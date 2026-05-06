/// @file At_MasterOutputEditor.cs
/// @brief Custom Inspector for At_MasterOutput.
///
/// @details
/// Handles audio device enumeration, speaker configuration, HRTF selection,
/// sampling rate / buffer size, real-time metering, and master gain control.
/// Device enumeration is cached and rate-limited to avoid blocking the Editor.

using System.Collections.Generic;
using UnityEngine;
using UnityEditor;
using System.IO;
using System.Runtime.InteropServices;
using UnityEngine.SceneManagement;
using UnityEditor.SceneManagement;
using System.Text.RegularExpressions;
using System;

#region Helper Classes
/// <summary>Stores enumerated information about a single audio device.</summary>
public class AudioDeviceInfo
{
    public int    index;
    public string name;
    public int    maxInputChannels;
    public int    maxOutputChannels;

    public override string ToString() =>
        $"[{index}] {name} ({maxInputChannels} in, {maxOutputChannels} out)";
}
#endregion

[ExecuteInEditMode]
[CanEditMultipleObjects]
[CustomEditor(typeof(At_MasterOutput))]
public class At_MasterOutputEditor : Editor
{
    #region Constants
    private const int    AUDIO_PLUGIN_OK         = 0;
    private const int    AUDIO_PLUGIN_ERROR       = 1;
    private const double DEVICE_REFRESH_COOLDOWN  = 5.0; // seconds
    #endregion

    #region Private State
    private int  selectedDeviceIndex = 0;
    private List<AudioDeviceInfo> availableDevices = new List<AudioDeviceInfo>();
    private string[] devices;

    private At_OutputState  outputState = new At_OutputState();
    private At_MasterOutput masterOutput;

    private readonly string[] outputConfigSelection = {
        "select", "1D", "2D SQUARE", "2D HALF-SQUARE", "2D CIRCLE", "2D HALF-CIRCLE", "Custom"
    };
    private readonly string[] samplingRateConfigSelection = { "44100", "48000" };
    private readonly string[] bufferSizeConfigSelection   = { "64", "128", "256", "512", "1024", "2048", "4096" };

    private int  selectSpeakerConfig  = 0;
    private int  outputConfigDimension = 0;
    private int  selectedSamplingRate  = 0;
    private int  selectedBufferSize    = 0;
    private int  samplingRate          = 44100;
    private int  bufferSize            = 512;
    private bool isStartingEngineOnAwake;
    private bool shouldSave = false;

    public GameObject[] speakers;

    private GUIStyle horizontalLine;
    private Texture  meterOn;
    private Texture  meterOff;
    public  float[]  meters;

    private static List<AudioDeviceInfo> cachedDevices      = null;
    private static bool   devicesNeedRefresh                = true;
    private static double lastDeviceRefreshTime             = 0;

    // Deferred spatconfig actions — set by the Load/Save buttons in
    // DrawSpeakerConfigSection and executed via EditorApplication.delayCall.
    // Calling OpenFilePanel / SaveFilePanel / DisplayDialog directly inside an
    // IMGUI layout pass triggers ExitGUIException, aborting the pass before all
    // EndHorizontal calls are reached → "EndLayoutGroup: BeginLayoutGroup must
    // be called first".  delayCall runs after the current pass completes.
    private bool m_pendingSpatConfigLoad = false;
    private bool m_pendingSpatConfigSave = false;

    // Deferred HRTF file open — set by the Load button, consumed via
    // EditorApplication.delayCall to avoid ExitGUIException from OpenFilePanel
    // aborting the layout pass before EndHorizontal / EndVertical are reached.
    private bool m_pendingHRTFLoad = false;

    // Deferred binaural auto-switch warning dialog.
    // Triggered whenever numVirtualSpeakers > physical device output channels
    // in non-binaural mode.  Instead of clamping, the engine is automatically
    // switched to binaural virtualization and the user is notified via a dialog
    // opened outside the current IMGUI layout pass (delayCall pattern).
    private bool   m_pendingBinauralAutoSwitch        = false;
    private string m_pendingBinauralAutoSwitchMessage = "";

    // True when this editor targets a duplicate At_MasterOutput pending
    // destruction via delayCall. Prevents OnInspectorGUI from accessing
    // uninitialized state in the frames before DestroyImmediate runs.
    private bool m_isDuplicate = false;
    #endregion

    #region Initialization
    public void OnEnable()
    {
        masterOutput = (At_MasterOutput)target;

        // ── Singleton guard: une seule instance de At_MasterOutput par scène ─
        // OnEnable est appelé à chaque fois qu'un composant est sélectionné OU
        // ajouté. Si plusieurs instances existent, c'est que l'utilisateur vient
        // d'en ajouter une seconde → on la supprime via delayCall pour éviter
        // tout conflit avec le pass IMGUI en cours.
        {
            At_MasterOutput[] all = FindObjectsOfType<At_MasterOutput>();
            if (all.Length > 1)
            {
                m_isDuplicate = true;
                At_MasterOutput duplicate = masterOutput;
                EditorApplication.delayCall += () =>
                {
                    if (duplicate == null) return;
                    string goName = duplicate.gameObject != null
                        ? duplicate.gameObject.name : "unknown";
                    EditorUtility.DisplayDialog(
                        "At_MasterOutput — Single instance only",
                        $"Only one At_MasterOutput is allowed per scene.\n\n" +
                        $"The component just added on \"{goName}\" has been removed.",
                        "OK");
                    DestroyImmediate(duplicate);
                };
                return; // ne pas initialiser un duplicata
            }
        }

        // Ensure At_Listener exists in the scene
        if (FindObjectOfType<At_Listener>() == null)
        {
            GameObject go = new GameObject("At_Listener");
            go.transform.position = Vector3.zero;
            go.AddComponent<At_Listener>();
        }

        horizontalLine = new GUIStyle
        {
            normal    = { background = EditorGUIUtility.whiteTexture },
            margin    = new RectOffset(0, 0, 4, 4),
            fixedHeight = 1
        };

        outputState = At_AudioEngineUtils.getOutputState(SceneManager.GetActiveScene().name);

        if (outputState == null)
        {
            outputState = new At_OutputState
            {
                audioDeviceName          = "",
                outputChannelCount       = 0,
                selectSpeakerConfig      = 0,
                outputConfigDimension    = 1,
                selectedSamplingRate     = 0,
                selectedBufferSize       = 0,
                samplingRate             = 48000,
                bufferSize               = 512,
                isStartingEngineOnAwake  = true,
                virtualSpeakerRigSize    = 3.0f,
                maxDistanceForDelay      = 10.0f,
                isBinauralVirtualization = false,
                isSimpleBinauralSpat     = false,
                numVirtualSpeakers       = 2
            };
            isStartingEngineOnAwake = true;
            samplingRate            = 48000;
            outputConfigDimension   = 1;
        }
        else
        {
            masterOutput.audioDeviceName          = outputState.audioDeviceName;
            masterOutput.outputChannelCount       = outputState.outputChannelCount;
            masterOutput.outputConfigDimension    = outputState.outputConfigDimension;
            masterOutput.gain                     = outputState.gain;
            masterOutput.makeupGain               = outputState.makeupGain;
            masterOutput.samplingRate             = outputState.samplingRate;
            masterOutput.bufferSize               = outputState.bufferSize;
            masterOutput.isStartingEngineOnAwake  = outputState.isStartingEngineOnAwake;
            masterOutput.virtualSpeakerRigSize    = outputState.virtualSpeakerRigSize;
            masterOutput.maxDistanceForDelay      = outputState.maxDistanceForDelay;
            masterOutput.isBinauralVirtualization = outputState.isBinauralVirtualization;
            masterOutput.isSimpleBinauralSpat     = outputState.isSimpleBinauralSpat;
            masterOutput.isNearFieldCorrection    = outputState.isNearFieldCorrection;
            masterOutput.hrtfDistance             = outputState.hrtfDistance;
            masterOutput.numVirtualSpeakers       = outputState.numVirtualSpeakers;

            selectSpeakerConfig     = outputState.selectSpeakerConfig;
            outputConfigDimension   = outputState.outputConfigDimension;
            samplingRate            = outputState.samplingRate;
            bufferSize              = outputState.bufferSize;
            selectedSamplingRate    = outputState.selectedSamplingRate;
            selectedBufferSize      = outputState.selectedBufferSize;
            isStartingEngineOnAwake = outputState.isStartingEngineOnAwake;

            if (masterOutput.outputChannelCount > 0)
            {
                meters = new float[masterOutput.outputChannelCount];
                for (int i = 0; i < meters.Length; i++) meters[i] = -80f;
            }
        }

        // In Play mode the engine is already running — skip the device scan entirely.
        // AT_WS_waitForDeviceScan() is a blocking call that freezes the main thread
        // for up to 10 s, which is the cause of the Inspector freeze on Windows.
        if (!Application.isPlaying && (cachedDevices == null || devicesNeedRefresh))
        {
            RefreshDeviceList();
            devicesNeedRefresh = false;
        }

        availableDevices = cachedDevices;
        if (availableDevices != null)
        {
            devices = new string[availableDevices.Count];
            for (int i = 0; i < availableDevices.Count; i++)
                devices[i] = availableDevices[i].name;
        }
    }

    public void OnDisable()
    {
        if (shouldSave)
        {
            At_AudioEngineUtils.SaveAllState(SceneManager.GetActiveScene().name);
            shouldSave = false;
        }
        At_AudioEngineUtils.CleanAllStates(SceneManager.GetActiveScene().name);
    }

    private void AffirmResources()
    {
        if (meterOn != null) return;
        meterOn  = Resources.Load<Texture>("At_WaveSpace/LevelMeter");
        meterOff = Resources.Load<Texture>("At_WaveSpace/LevelMeterOff");
    }
    #endregion

    #region Device Enumeration
    private List<AudioDeviceInfo> EnumerateDevices()
    {
        availableDevices.Clear();

        int deviceCount = AT_WS_getDeviceCount();
        if (deviceCount <= 0) { Debug.LogError("[AudioPlugin] Failed to get device count"); return availableDevices; }

        for (int i = 0; i < deviceCount; i++)
        {
            IntPtr nameBuffer = Marshal.AllocHGlobal(256);
            IntPtr typeBuffer = Marshal.AllocHGlobal(64);
            try
            {
                int maxIn = 0, maxOut = 0;
                if (AT_WS_getCachedDeviceInfo(i, nameBuffer, typeBuffer, ref maxIn, ref maxOut) != AUDIO_PLUGIN_OK)
                    continue;
                if (maxOut <= 0)
                    continue;
                availableDevices.Add(new AudioDeviceInfo
                {
                    index             = i,
                    name              = Marshal.PtrToStringAnsi(nameBuffer),
                    maxInputChannels  = maxIn,
                    maxOutputChannels = maxOut
                });
            }
            finally
            {
                Marshal.FreeHGlobal(nameBuffer);
                Marshal.FreeHGlobal(typeBuffer);
            }
        }
        return availableDevices;
    }

    private void RefreshDeviceList()
    {
        AT_WS_initialize();
        AT_WS_waitForDeviceScan(10000);
        AT_WS_filterUnavailableDevices();
        cachedDevices         = EnumerateDevices();
        devicesNeedRefresh    = false;
        lastDeviceRefreshTime = EditorApplication.timeSinceStartup;
    }

    private int GetSelectedDeviceMaxOutputChannels()
    {
        if (availableDevices == null || availableDevices.Count == 0 ||
            selectedDeviceIndex < 0  || selectedDeviceIndex >= availableDevices.Count)
            return 2;

        AudioDeviceInfo info = availableDevices[selectedDeviceIndex];
        if (info.maxOutputChannels == 0)  return 2;
        if (info.maxOutputChannels != -1) return info.maxOutputChannels;

        // Lazy detailed query
        IntPtr nameBuffer = Marshal.AllocHGlobal(256);
        IntPtr typeBuffer = Marshal.AllocHGlobal(64);
        try
        {
            int maxIn = 0, maxOut = 0;
            if (AT_WS_getDetailedDeviceInfo(info.index, nameBuffer, typeBuffer, ref maxIn, ref maxOut) == AUDIO_PLUGIN_OK)
            {
                info.maxInputChannels  = maxIn;
                info.maxOutputChannels = maxOut;
                return maxOut > 0 ? maxOut : 2;
            }
        }
        finally
        {
            Marshal.FreeHGlobal(nameBuffer);
            Marshal.FreeHGlobal(typeBuffer);
        }
        return 2;
    }
    #endregion

    #region Inspector GUI
    public override void OnInspectorGUI()
    {
        // Guard: ne rien dessiner si ce composant est un duplicata en attente
        // de destruction (delayCall pas encore exécuté). Evite la NullReference
        // sur horizontalLine / outputState non initialisés.
        if (m_isDuplicate || masterOutput == null) return;

        AffirmResources();

        bool speakerConfigChanged = false;

        if (!Application.isPlaying || !masterOutput.isPlaying)
            DrawEditModeGUI(ref speakerConfigChanged);
        else
            DrawPlayModeGUI();

        outputState.gain       = masterOutput.gain;
        outputState.makeupGain = masterOutput.makeupGain;

        if (speakerConfigChanged) RegenerateSpeakerConfiguration();
    }

    private void DrawEditModeGUI(ref bool speakerConfigChanged)
    {
        GUILayout.Space(5);
        DrawDeviceSelectionSection();
        DrawMetersAndGainSection();
        HorizontalLine(Color.black);
        DrawBinauralVirtualizationSection();
        HorizontalLine(Color.black);
        DrawSamplingRateAndBufferSection();
        HorizontalLine(Color.black);
        DrawSpeakerConfigSection(ref speakerConfigChanged);
        HorizontalLine(Color.black);
        DrawMaxDistanceSection();
        HorizontalLine(Color.black);
        DrawCleanButtonSection();
        UpdateMasterOutputParameters();
    }

    private void DrawPlayModeGUI()
    {
        GUILayout.Space(10);
        EditorGUILayout.LabelField($"WFS Channels Output: [1 - {masterOutput.outputChannelCount}]", EditorStyles.boldLabel);
        GUILayout.Space(5);
        DrawMetersAndGainSection();
        GUILayout.Space(6);
        EditorGUILayout.HelpBox("Player and binaural settings →  AT_WaveSpace → Settings…", MessageType.Info);
    }

    private void DrawDeviceSelectionSection()
    {
        double timeSinceRefresh = EditorApplication.timeSinceStartup - lastDeviceRefreshTime;
        bool   canRefresh       = timeSinceRefresh >= DEVICE_REFRESH_COOLDOWN;
        string btnLabel         = canRefresh
            ? "🔄 Refresh Devices"
            : $"🔄 Refresh ({(DEVICE_REFRESH_COOLDOWN - timeSinceRefresh):F0}s)";

        EditorGUILayout.BeginHorizontal();
        GUILayout.FlexibleSpace();
        GUI.enabled = canRefresh;
        if (GUILayout.Button(btnLabel, GUILayout.Width(150)))
        {
            devicesNeedRefresh = true;
            RefreshDeviceList();
            if (availableDevices != null)
            {
                devices = new string[availableDevices.Count];
                for (int i = 0; i < availableDevices.Count; i++) devices[i] = availableDevices[i].name;
                selectedDeviceIndex = 0;
                for (int i = 0; i < devices.Length; i++)
                    if (devices[i] == outputState.audioDeviceName) { selectedDeviceIndex = i; break; }
            }
        }
        GUI.enabled = true;
        GUILayout.FlexibleSpace();
        EditorGUILayout.EndHorizontal();

        GUILayout.Space(5);
        HorizontalLine(Color.black);
        GUILayout.Space(5);

        EditorGUILayout.BeginHorizontal();
        GUILayout.Label("Audio Device:", GUILayout.Width(120));
        if (devices != null && devices.Length != 0)
        {
            for (int i = 0; i < devices.Length; i++)
                if (devices[i] == outputState.audioDeviceName) { selectedDeviceIndex = i; break; }

            int newIndex = EditorGUILayout.Popup(selectedDeviceIndex, devices);
            if (newIndex != selectedDeviceIndex) { selectedDeviceIndex = newIndex; shouldSave = true; }

            outputState.audioDeviceName = devices[selectedDeviceIndex];
        }
        EditorGUILayout.EndHorizontal();

        if (devices != null && devices.Length != 0)
        {
            int maxCh = GetSelectedDeviceMaxOutputChannels();
            EditorGUILayout.BeginHorizontal();
            GUILayout.Space(20);
            GUILayout.Label($"Max Output Channels: {maxCh}", EditorStyles.helpBox);
            EditorGUILayout.EndHorizontal();
        }
        GUILayout.Space(5);
    }

    private void DrawSamplingRateAndBufferSection()
    {
        GUILayout.Space(5);

        EditorGUILayout.BeginHorizontal();
        GUILayout.Label("Buffer Size:", GUILayout.Width(120));
        int newBufIdx = EditorGUILayout.Popup(selectedBufferSize, bufferSizeConfigSelection);
        bufferSize = int.Parse(bufferSizeConfigSelection[newBufIdx]);
        if (bufferSize != outputState.bufferSize)
        {
            selectedBufferSize          = newBufIdx;
            outputState.bufferSize      = bufferSize;
            outputState.selectedBufferSize = selectedBufferSize;
            shouldSave = true;
        }
        EditorGUILayout.EndHorizontal();

        GUILayout.Space(5);

        EditorGUILayout.BeginHorizontal();
        GUILayout.Label("Sampling Rate:", GUILayout.Width(120));
        int newSrIdx = EditorGUILayout.Popup(selectedSamplingRate, samplingRateConfigSelection);
        samplingRate = newSrIdx == 0 ? 44100 : 48000;
        if (samplingRate != outputState.samplingRate)
        {
            selectedSamplingRate            = newSrIdx;
            outputState.samplingRate        = samplingRate;
            outputState.selectedSamplingRate = selectedSamplingRate;
            shouldSave = true;
        }
        EditorGUILayout.EndHorizontal();

        GUILayout.Space(5);
    }

    private void DrawSpeakerConfigSection(ref bool speakerConfigChanged)
    {
        GUILayout.Space(5);
        EditorGUILayout.BeginHorizontal();
        GUILayout.Label("Speaker Configuration:", GUILayout.Width(150));
        int newConfig = EditorGUILayout.Popup(selectSpeakerConfig, outputConfigSelection);
        EditorGUILayout.EndHorizontal();

        // ── Custom configuration (index 6) ────────────────────────────────
        if (newConfig == 6)
        {
            if (newConfig != selectSpeakerConfig)
            {
                selectSpeakerConfig             = newConfig;
                outputState.selectSpeakerConfig = newConfig;
                shouldSave                      = true;
            }

            GUILayout.Space(4);
            EditorGUILayout.BeginHorizontal();
            GUILayout.FlexibleSpace();
            if (GUILayout.Button("📂 Load Speaker Config", GUILayout.Width(190), GUILayout.Height(28)))
            {
                m_pendingSpatConfigLoad = true;
                EditorApplication.delayCall += ExecutePendingSpatConfigLoad;
            }
            GUILayout.FlexibleSpace();
            EditorGUILayout.EndHorizontal();

            GUILayout.Space(4);
            EditorGUILayout.BeginHorizontal();
            GUILayout.FlexibleSpace();
            if (GUILayout.Button("💾 Save Speaker Config", GUILayout.Width(190), GUILayout.Height(28)))
            {
                m_pendingSpatConfigSave = true;
                EditorApplication.delayCall += ExecutePendingSpatConfigSave;
            }
            GUILayout.FlexibleSpace();
            EditorGUILayout.EndHorizontal();

            GUILayout.Label("Load: charge un fichier .spatconfig depuis StreamingAssets/SpatConfig.\\n"
                          + "Save: exporte la config actuelle vers un .spatconfig de votre choix.",
                            EditorStyles.wordWrappedMiniLabel);
            GUILayout.Space(4);

            // Read-only display of values sourced from the last JSON load
            EditorGUILayout.BeginHorizontal();
            GUILayout.Label("Virtual Speakers:", GUILayout.Width(150));
            GUILayout.Label(outputState.numVirtualSpeakers.ToString(), EditorStyles.helpBox, GUILayout.Width(60));
            EditorGUILayout.EndHorizontal();

            EditorGUILayout.BeginHorizontal();
            GUILayout.Label("Speaker Rig Size:", GUILayout.Width(150));
            GUILayout.Label($"{outputState.virtualSpeakerRigSize:F2} m  (first ↔ last)", EditorStyles.helpBox);
            EditorGUILayout.EndHorizontal();
        }
        // ── Standard configurations (indices 1-5) ─────────────────────────
        else
        {
            if (newConfig != 0)
            {
                outputConfigDimension = newConfig;
                selectSpeakerConfig   = newConfig;

                EditorGUILayout.BeginHorizontal();
                GUILayout.Space(20);
                GUILayout.Label("Num. Virtual Speakers:", GUILayout.Width(130));
                int numVS = EditorGUILayout.IntField(outputState.numVirtualSpeakers);
                EditorGUILayout.EndHorizontal();

                int deviceMax = GetSelectedDeviceMaxOutputChannels();
                if (!outputState.isBinauralVirtualization && numVS > deviceMax)
                {
                    // Automatically switch to binaural virtualization instead of
                    // silently clamping — clamping would leave speakers inactive
                    // with no audio feedback for the user.
                    SwitchToBinauralWithWarning(numVS, deviceMax);
                }

                bool invalid = numVS <= 1 || numVS > 1024
                    || (outputConfigDimension == 2 && numVS % 4 != 0)
                    || (outputConfigDimension == 3 && numVS < 3);

                if (invalid)
                {
                    numVS                 = outputState.numVirtualSpeakers;
                    outputConfigDimension = outputState.outputConfigDimension;
                }

                if (numVS > 1 && (numVS != outputState.numVirtualSpeakers || outputConfigDimension != outputState.outputConfigDimension))
                {
                    speakerConfigChanged = true;
                    shouldSave           = true;

                    outputState.outputConfigDimension = outputConfigDimension;
                    outputState.selectSpeakerConfig   = selectSpeakerConfig;
                    outputState.numVirtualSpeakers    = numVS;
                    masterOutput.numVirtualSpeakers   = numVS;

                    int channelCount = outputState.isBinauralVirtualization ? 2 : numVS;
                    outputState.outputChannelCount  = channelCount;
                    masterOutput.outputChannelCount = channelCount;

                    foreach (At_Player p in FindObjectsOfType<At_Player>())
                        p.outputChannelCount = numVS;

                    meters = new float[channelCount];
                }
            }

            // Rig size — editable slider for standard configs
            EditorGUILayout.BeginHorizontal();
            GUILayout.Label("Speaker Rig Size:", GUILayout.Width(150));
            string sizeStr = EditorGUILayout.TextField(outputState.virtualSpeakerRigSize.ToString("F1"), GUILayout.Width(60));
            EditorGUILayout.EndHorizontal();

            EditorGUILayout.BeginHorizontal();
            float size = GUILayout.HorizontalSlider(outputState.virtualSpeakerRigSize, 0.1f, 80f);
            EditorGUILayout.EndHorizontal();

            if (float.TryParse(sizeStr, out float parsedSize))
            {
                parsedSize = Mathf.Clamp(parsedSize, 0.1f, 80f);
                if (parsedSize != outputState.virtualSpeakerRigSize) size = parsedSize;
            }

            if (size != outputState.virtualSpeakerRigSize)
            {
                outputState.virtualSpeakerRigSize  = size;
                masterOutput.virtualSpeakerRigSize = size;
                shouldSave                         = true;
                speakerConfigChanged               = true;
            }
        }

        GUILayout.Space(15);
    }

    /// <summary>
    /// Reads StreamingAssets/SpatialConfiguration.json (via <see cref="At_SpatialConfigState"/>)
    /// and permanently positions all AT_VirtualSpeaker GameObjects and the AT_Listener.
    /// Also updates numVirtualSpeakers, virtualSpeakerRigSize and outputChannelCount
    /// in both outputState and the masterOutput instance.
    /// </summary>
    private void ApplyCustomSpeakerConfiguration()
    {
        // ── 1. Browse & load .spatconfig ──────────────────────────────────
        string spatConfigDir = Path.Combine(Application.streamingAssetsPath, "SpatConfig");
        if (!Directory.Exists(spatConfigDir))
        {
            Directory.CreateDirectory(spatConfigDir);
            AssetDatabase.Refresh();
        }

        // Show all files — OpenFilePanelWithFilters and OpenFilePanel(ext) both fail
        // to display non-registered extensions on Windows.  Extension validation is
        // done manually after selection.
        string jsonPath = EditorUtility.OpenFilePanel("Load Speaker Config", spatConfigDir, "");
        if (string.IsNullOrEmpty(jsonPath)) return;

        if (!jsonPath.EndsWith(".spatconfig", System.StringComparison.OrdinalIgnoreCase))
        {
            EditorUtility.DisplayDialog("Wrong File Type",
                $"'{Path.GetFileName(jsonPath)}' is not a .spatconfig file.\n" +
                "Please select a valid speaker configuration file.", "OK");
            return;
        }

        At_SpatialConfigState config;
        try   { config = JsonUtility.FromJson<At_SpatialConfigState>(File.ReadAllText(jsonPath)); }
        catch (Exception e)
        {
            EditorUtility.DisplayDialog("Parse Error",
                "Failed to parse " + Path.GetFileName(jsonPath) + ":\n" + e.Message, "OK");
            return;
        }

        int n = config.numSpeakerPosition;
        if (n <= 0 || config.speaker_posx == null || config.speaker_posx.Length < n)
        {
            EditorUtility.DisplayDialog("Invalid Config",
                "No valid speaker data in " + Path.GetFileName(jsonPath) + ".", "OK");
            return;
        }

        // ── 1b. Determine effective virtual speaker count ─────────────────
        // In binaural mode numVirtualSpeakers is decoupled from physical output
        // channels (physical output is always 2).  All n speakers are loaded into
        // the scene and the engine creates n HRTF processors.
        // In non-binaural mode numVirtualSpeakers equals physical output channels,
        // so it must not exceed the device maximum.  We still create all n speaker
        // GameObjects in the scene (for correct visualization and so the full rig
        // is ready when binaural is later enabled), but we clamp the effective
        // count passed to the engine.
        int effectiveVS = n;
        if (!outputState.isBinauralVirtualization)
        {
            int deviceMax = GetSelectedDeviceMaxOutputChannels();
            if (deviceMax > 0 && n > deviceMax)
            {
                // Automatically switch to binaural virtualization so all n virtual
                // speakers are active regardless of the physical output channel count.
                SwitchToBinauralWithWarning(n, deviceMax);
                // effectiveVS stays n — binaural is now ON, no channel count limit applies.
            }
        }

        // Store the raw config count before any clamping — used to restore the
        // full virtual speaker count when binaural virtualization is later enabled.
        outputState.numVirtualSpeakersConfig = n;

        // ── 2. Destroy existing VirtualSpeakers & parent ──────────────────
        At_VirtualSpeaker[] existingVss = FindObjectsOfType<At_VirtualSpeaker>();
        GameObject vsParent = (existingVss.Length > 0) ? existingVss[0].transform.parent?.gameObject : null;
        foreach (At_VirtualSpeaker vs in existingVss) DestroyImmediate(vs.gameObject);
        if (vsParent != null) DestroyImmediate(vsParent);

        // ── 3. Create VirtualSpeaker GameObjects ──────────────────────────
        GameObject virtualSpkParent = new GameObject("VirtualSpeakers");
        virtualSpkParent.transform.SetParent(masterOutput.gameObject.transform, worldPositionStays: false);

        for (int i = 0; i < n; i++)
        {
            string name = (config.speaker_name != null && i < config.speaker_name.Length)
                ? config.speaker_name[i] : "spk_" + (i + 1);

            GameObject spkGO = new GameObject(name);
            spkGO.transform.SetParent(virtualSpkParent.transform, worldPositionStays: false);

            At_VirtualSpeaker vs   = spkGO.AddComponent<At_VirtualSpeaker>();
            vs.id                  = i;   // 0-based array index
            vs.m_maxDistanceForDelay = outputState.maxDistanceForDelay;

            spkGO.transform.position = new Vector3(
                config.speaker_posx[i],
                config.speaker_posy[i],
                config.speaker_posz[i]);

            Vector3 fwd = new Vector3(
                config.speaker_fwdx[i],
                config.speaker_fwdy[i],
                config.speaker_fwdz[i]);
            if (fwd.sqrMagnitude > 0.0001f)
                spkGO.transform.rotation = Quaternion.LookRotation(fwd.normalized);
        }

        // ── 4. Rig size = distance between first and last speaker ─────────
        Vector3 posFirst = new Vector3(config.speaker_posx[0],     config.speaker_posy[0],     config.speaker_posz[0]);
        Vector3 posLast  = new Vector3(config.speaker_posx[n - 1], config.speaker_posy[n - 1], config.speaker_posz[n - 1]);
        float   rigSize  = Mathf.Max(Vector3.Distance(posFirst, posLast), 0.1f);

        // ── 5. Update counts ──────────────────────────────────────────────
        int channelCount = outputState.isBinauralVirtualization ? 2 : effectiveVS;

        outputState.numVirtualSpeakers    = effectiveVS;
        outputState.outputChannelCount    = channelCount;
        outputState.outputConfigDimension = 0;
        outputState.selectSpeakerConfig   = 6;
        outputState.virtualSpeakerRigSize = rigSize;

        masterOutput.numVirtualSpeakers    = effectiveVS;
        masterOutput.outputChannelCount    = channelCount;
        masterOutput.outputConfigDimension = 0;
        masterOutput.virtualSpeakerRigSize = rigSize;
        masterOutput.virtualSpeakers       = FindObjectsOfType<At_VirtualSpeaker>();

        foreach (At_Player p in FindObjectsOfType<At_Player>())
            p.outputChannelCount = effectiveVS;

        meters = new float[channelCount];

        // ── 6. Create source GameObjects (empty placeholders) ─────────────────
        // Sources described in the .spatconfig are materialized as plain
        // GameObjects so their positions are immediately visible in the Scene
        // view.  They intentionally have no At_Player component on import;
        // the user can add one manually when needed.  A subsequent
        // SaveCustomSpeakerConfiguration() will then capture them automatically
        // (it collects every GameObject that carries an At_Player component).
        //
        // Existing source objects from a previous load are destroyed first so
        // we never accumulate duplicates across successive loads.
        GameObject existingSourceParent = GameObject.Find("Sources");
        if (existingSourceParent != null)
            DestroyImmediate(existingSourceParent);

        if (config.numSourcePosition > 0
            && config.source_posx != null
            && config.source_posx.Length >= config.numSourcePosition)
        {
            int nSrc = config.numSourcePosition;
            GameObject sourceParent = new GameObject("Sources");

            for (int i = 0; i < nSrc; i++)
            {
                string srcName = (config.source_name != null && i < config.source_name.Length
                                  && !string.IsNullOrEmpty(config.source_name[i]))
                    ? config.source_name[i]
                    : "source_" + i;

                GameObject srcGO = new GameObject(srcName);
                srcGO.transform.SetParent(sourceParent.transform, worldPositionStays: false);
                srcGO.transform.position = new Vector3(
                    config.source_posx[i],
                    config.source_posy[i],
                    config.source_posz[i]);
            }
        }

        // ── 7. Sync editor state & save ───────────────────────────────────────────
        selectSpeakerConfig = 6;
        shouldSave          = true;
        EditorSceneManager.SaveScene(SceneManager.GetActiveScene());
    }

    /// <summary>
    /// Exports the current AT_VirtualSpeaker positions and orientations from the scene
    /// to StreamingAssets/SpatialConfiguration.json.
    /// </summary>
    private void SaveCustomSpeakerConfiguration()
    {
        At_VirtualSpeaker[] vss = FindObjectsOfType<At_VirtualSpeaker>();
        if (vss == null || vss.Length == 0)
        {
            EditorUtility.DisplayDialog("No Speakers",
                "No AT_VirtualSpeaker found in the scene.", "OK");
            return;
        }

        // Sort by id to guarantee consistent ordering
        System.Array.Sort(vss, (a, b) => a.id.CompareTo(b.id));
        int n = vss.Length;

        // ── 2. Collect At_Player source positions ─────────────────────────────
        // Every GameObject that carries an At_Player component is treated as a
        // source.  Its world position and name are exported alongside the
        // speaker data so the .spatconfig round-trips cleanly through
        // ApplyCustomSpeakerConfiguration.
        At_Player[] players = FindObjectsOfType<At_Player>();

        // Sort by GameObject name for stable ordering across saves
        System.Array.Sort(players, (a, b) =>
            string.Compare(a.gameObject.name, b.gameObject.name,
                           System.StringComparison.OrdinalIgnoreCase));

        int nSrc = players.Length;

        At_SpatialConfigState config = new At_SpatialConfigState
        {
            numSpeakerPosition = n,
            speaker_name = new string[n],
            speaker_posx = new float[n],
            speaker_posy = new float[n],
            speaker_posz = new float[n],
            speaker_fwdx = new float[n],
            speaker_fwdy = new float[n],
            speaker_fwdz = new float[n],

            numSourcePosition = nSrc,
            source_name = new string[nSrc],
            source_posx = new float[nSrc],
            source_posy = new float[nSrc],
            source_posz = new float[nSrc],
        };

        for (int i = 0; i < n; i++)
        {
            config.speaker_name[i] = vss[i].gameObject.name;
            config.speaker_posx[i] = vss[i].transform.position.x;
            config.speaker_posy[i] = vss[i].transform.position.y;
            config.speaker_posz[i] = vss[i].transform.position.z;
            config.speaker_fwdx[i] = vss[i].transform.forward.x;
            config.speaker_fwdy[i] = vss[i].transform.forward.y;
            config.speaker_fwdz[i] = vss[i].transform.forward.z;
        }

        for (int i = 0; i < nSrc; i++)
        {
            config.source_name[i] = players[i].gameObject.name;
            config.source_posx[i] = players[i].transform.position.x;
            config.source_posy[i] = players[i].transform.position.y;
            config.source_posz[i] = players[i].transform.position.z;
        }

        string spatConfigDir = Path.Combine(Application.streamingAssetsPath, "SpatConfig");
        if (!Directory.Exists(spatConfigDir))
        {
            Directory.CreateDirectory(spatConfigDir);
            AssetDatabase.Refresh();
        }

        string savePath = EditorUtility.SaveFilePanel(
            "Save Speaker Config", spatConfigDir, "SpeakerConfig", "spatconfig");
        if (string.IsNullOrEmpty(savePath)) return;

        // SaveFilePanel does not append the extension on Windows for non-registered
        // file types.  Ensure the path always ends with .spatconfig regardless of
        // what the OS dialog returned.
        if (!savePath.EndsWith(".spatconfig", System.StringComparison.OrdinalIgnoreCase))
            savePath += ".spatconfig";

        try
        {
            File.WriteAllText(savePath, JsonUtility.ToJson(config, true));
            AssetDatabase.Refresh();
            string fname  = Path.GetFileName(savePath);
            string folder = Path.GetFileName(Path.GetDirectoryName(savePath));
            // Non-modal floating notification — fades automatically without user interaction.
            SceneView.lastActiveSceneView?.ShowNotification(
                new GUIContent($"✓ {fname} saved in {folder}/"), 2.0);
        }
        catch (Exception e)
        {
            EditorUtility.DisplayDialog("Save Error",
                "Failed to write file:\n" + e.Message, "OK");
        }
    }

    private void DrawMaxDistanceSection()
    {
        GUILayout.Space(5);
        EditorGUILayout.BeginHorizontal();
        GUILayout.Label("Max Distance/Delay (m):", GUILayout.Width(200));
        string distStr = EditorGUILayout.TextField(outputState.maxDistanceForDelay.ToString("F1"), GUILayout.Width(60));
        EditorGUILayout.EndHorizontal();

        EditorGUILayout.BeginHorizontal();
        float dist = GUILayout.HorizontalSlider(outputState.maxDistanceForDelay, 10f, 100f);
        EditorGUILayout.EndHorizontal();

        if (float.TryParse(distStr, out float parsedDist))
        {
            parsedDist = Mathf.Clamp(parsedDist, 10f, 100f);
            if (parsedDist != outputState.maxDistanceForDelay) dist = parsedDist;
        }

        if (dist != outputState.maxDistanceForDelay)
        {
            outputState.maxDistanceForDelay  = dist;
            masterOutput.maxDistanceForDelay = dist;
            foreach (At_VirtualSpeaker vs in FindObjectsOfType<At_VirtualSpeaker>())
                vs.m_maxDistanceForDelay = dist;
            shouldSave = true;
        }

        GUILayout.Space(15);
    }

    private void DrawMetersAndGainSection()
    {
        GUILayout.Label("Gain", EditorStyles.boldLabel);

        // ── Master Gain ───────────────────────────────────────────────────────
        EditorGUILayout.BeginHorizontal();
        GUILayout.Label("Master Gain (dB)", GUILayout.Width(150));
        string gainStr = EditorGUILayout.TextField(masterOutput.gain.ToString("F1"), GUILayout.Width(60));
        if (float.TryParse(gainStr,
                System.Globalization.NumberStyles.Float,
                System.Globalization.CultureInfo.InvariantCulture,
                out float pg))
        {
            pg = Mathf.Clamp(pg, -80f, 10f);
            if (!Mathf.Approximately(pg, masterOutput.gain))
            {
                masterOutput.gain  = pg;
                outputState.gain   = pg;
                shouldSave         = true;
            }
        }
        GUILayout.Label("dB", EditorStyles.miniLabel);
        EditorGUILayout.EndHorizontal();

        EditorGUILayout.BeginHorizontal();
        float newGain = GUILayout.HorizontalSlider(masterOutput.gain, -80f, 10f);
        if (!Mathf.Approximately(newGain, masterOutput.gain))
        {
            masterOutput.gain  = newGain;
            outputState.gain   = newGain;
            shouldSave         = true;
        }
        EditorGUILayout.EndHorizontal();
        GUILayout.Space(15);

        // ── Makeup Gain ───────────────────────────────────────────────────────
        EditorGUILayout.BeginHorizontal();
        GUILayout.Label("Makeup Gain (dB)", GUILayout.Width(150));
        string makeupStr = EditorGUILayout.TextField(masterOutput.makeupGain.ToString("F1"), GUILayout.Width(60));
        if (float.TryParse(makeupStr,
                System.Globalization.NumberStyles.Float,
                System.Globalization.CultureInfo.InvariantCulture,
                out float pmg))
        {
            pmg = Mathf.Clamp(pmg, -10f, 40f);
            if (!Mathf.Approximately(pmg, masterOutput.makeupGain))
            {
                masterOutput.makeupGain  = pmg;
                outputState.makeupGain   = pmg;
                shouldSave               = true;
            }
        }
        GUILayout.Label("dB", EditorStyles.miniLabel);
        EditorGUILayout.EndHorizontal();        

        EditorGUILayout.BeginHorizontal();
        float newMakeup = GUILayout.HorizontalSlider(masterOutput.makeupGain, -10f, 40f);
        if (!Mathf.Approximately(newMakeup, masterOutput.makeupGain))
        {
            masterOutput.makeupGain  = newMakeup;
            outputState.makeupGain   = newMakeup;
            shouldSave               = true;
        }
        EditorGUILayout.EndHorizontal();
        GUILayout.Space(15);
    }

    private void DrawBinauralSimpleBinauralABSection()
    {
        if (!outputState.isBinauralVirtualization && !masterOutput.isBinauralVirtualization) return;

        GUILayout.Space(10);
        EditorGUILayout.BeginHorizontal();
        GUILayout.Label("Simple Binaural Mode (A/B Test)", GUILayout.Width(200));
        bool newVal = EditorGUILayout.Toggle(outputState.isSimpleBinauralSpat, GUILayout.Width(20));
        if (newVal != outputState.isSimpleBinauralSpat)
        {
            outputState.isSimpleBinauralSpat  = newVal;
            masterOutput.isSimpleBinauralSpat = newVal;
            shouldSave = true;
        }
        EditorGUILayout.EndHorizontal();
        GUILayout.Label("Bypasses WFS and applies HRTF directly to each source (A/B comparison).",
            EditorStyles.wordWrappedMiniLabel);
    }

    private void DrawNearFieldCorrectionSection()
    {
        if (!outputState.isBinauralVirtualization && !masterOutput.isBinauralVirtualization) return;

        GUILayout.Space(6);
        EditorGUILayout.BeginHorizontal();
        GUILayout.Label("Near-Field Correction (NFC)", GUILayout.Width(200));
        bool newNfc = EditorGUILayout.Toggle(outputState.isNearFieldCorrection, GUILayout.Width(20));
        if (newNfc != outputState.isNearFieldCorrection)
        {
            outputState.isNearFieldCorrection  = newNfc;
            masterOutput.isNearFieldCorrection = newNfc;
            shouldSave = true;
        }
        EditorGUILayout.EndHorizontal();

        // HRTF reference distance (rRef) — only useful when NFC is on, but always visible
        // so it can be configured before enabling the correction.
        EditorGUILayout.BeginHorizontal();
        GUILayout.Label("HRTF Distance (m)", GUILayout.Width(200));
        string distStr = EditorGUILayout.TextField(
            outputState.hrtfDistance.ToString("F2"), GUILayout.Width(60));
        if (float.TryParse(distStr, System.Globalization.NumberStyles.Float,
                           System.Globalization.CultureInfo.InvariantCulture, out float newDist)
            && newDist > 0f
            && !Mathf.Approximately(newDist, outputState.hrtfDistance))
        {
            outputState.hrtfDistance  = newDist;
            masterOutput.hrtfDistance = newDist;
            shouldSave = true;
        }
        GUILayout.Label("m", GUILayout.Width(15));
        EditorGUILayout.EndHorizontal();

        GUILayout.Label(
            "Applies a near-field IIR correction (DVF, Duda & Martens 1998) to the binaural output. " +
            "HRTF Distance is the measurement distance of the loaded BRIR set (typically 1.0 m). " +
            "Source distance and azimuth are computed automatically from scene transforms.",
            EditorStyles.wordWrappedMiniLabel);
    }

    private void DrawBinauralVirtualizationSection()
    {
        EditorGUILayout.BeginHorizontal();
        GUILayout.Label("Binaural Virtualization", GUILayout.Width(150));
        bool newBin = EditorGUILayout.Toggle(outputState.isBinauralVirtualization, GUILayout.Width(20));
        if (newBin != outputState.isBinauralVirtualization)
        {
            outputState.isBinauralVirtualization  = newBin;
            masterOutput.isBinauralVirtualization = newBin;

            if (newBin)
            {
                // Binaural ON: physical output is always 2 channels regardless of
                // virtual speaker count — no channel count check needed.
                outputState.outputChannelCount  = 2;
                masterOutput.outputChannelCount = 2;

                // Restore the full virtual speaker count from the last loaded
                // .spatconfig, which may have been clamped to the device maximum
                // when binaural was previously off.
                if (outputState.selectSpeakerConfig == 6 &&
                    outputState.numVirtualSpeakersConfig > outputState.numVirtualSpeakers)
                {
                    outputState.numVirtualSpeakers  = outputState.numVirtualSpeakersConfig;
                    masterOutput.numVirtualSpeakers = outputState.numVirtualSpeakersConfig;
                    foreach (At_Player p in FindObjectsOfType<At_Player>())
                        p.outputChannelCount = outputState.numVirtualSpeakersConfig;
                }
            }
            else
            {
                // Binaural OFF: virtual speakers would map 1-to-1 to physical channels.
                // If numVirtualSpeakers exceeds the device maximum, revert the toggle
                // instead of clamping — clamping would silently discard speakers and
                // produce unexpected silence in WFS mode.
                int targetVS  = (outputState.selectSpeakerConfig == 6 && outputState.numVirtualSpeakersConfig > 0)
                    ? outputState.numVirtualSpeakersConfig
                    : outputState.numVirtualSpeakers;
                int deviceMax = GetSelectedDeviceMaxOutputChannels();
                if (deviceMax > 0 && targetVS > deviceMax)
                {
                    // Revert the toggle — keep binaural ON.
                    outputState.isBinauralVirtualization  = true;
                    masterOutput.isBinauralVirtualization = true;
                    outputState.outputChannelCount        = 2;
                    masterOutput.outputChannelCount       = 2;
                    SwitchToBinauralWithWarning(targetVS, deviceMax);
                }
                else
                {
                    outputState.numVirtualSpeakers  = targetVS;
                    masterOutput.numVirtualSpeakers = targetVS;
                    outputState.outputChannelCount  = targetVS;
                    masterOutput.outputChannelCount = targetVS;
                }
            }

            shouldSave = true;
        }
        EditorGUILayout.EndHorizontal();

        // Show an inline notice when binaural cannot be disabled because the
        // virtual speaker count exceeds the physical output channel count of
        // the selected device.  The notice is displayed both when binaural is
        // already ON (to explain the lock) and when binaural is OFF but the
        // condition is met (to explain why it was auto-switched).
        {
            int targetVS  = (outputState.selectSpeakerConfig == 6 && outputState.numVirtualSpeakersConfig > 0)
                ? outputState.numVirtualSpeakersConfig
                : outputState.numVirtualSpeakers;
            int deviceMax = GetSelectedDeviceMaxOutputChannels();
            if (deviceMax > 0 && targetVS > deviceMax)
            {
                EditorGUILayout.HelpBox(
                    $"Binaural Virtualization cannot be disabled: {targetVS} Virtual Speakers " +
                    $"exceed the {deviceMax} physical output channels of the selected device.\n" +
                    "To disable it: reduce \"Num. Virtual Speakers\" or select a device " +
                    "with enough physical output channels.",
                    MessageType.Warning);
            }
        }

        if (!outputState.isBinauralVirtualization) return;

        EditorGUILayout.BeginVertical("box");

        EditorGUILayout.BeginHorizontal();
        GUILayout.Label("HRTF File:", GUILayout.Width(80));
        // Display the relative path (e.g. "HRTF/MySet.txt") or a placeholder
        string displayPath = string.IsNullOrEmpty(outputState.hrtfFilePath)
            ? "[None - Using Default]"
            : outputState.hrtfFilePath;
        GUILayout.Label(displayPath, EditorStyles.wordWrappedLabel);
        EditorGUILayout.EndHorizontal();

        EditorGUILayout.BeginHorizontal();
        GUILayout.FlexibleSpace();
        if (GUILayout.Button("Load HRTF File (.txt)", GUILayout.Width(150), GUILayout.Height(25)))
        {
            // Defer OpenFilePanel outside the current layout pass.
            // Calling it directly triggers ExitGUIException, which aborts the pass
            // before EndHorizontal and EndVertical are reached.
            m_pendingHRTFLoad = true;
            EditorApplication.delayCall += ExecutePendingHRTFLoad;
        }
        if (GUILayout.Button("Use Default HRTF", GUILayout.Width(150), GUILayout.Height(25)))
        {
            outputState.hrtfFilePath  = "";
            masterOutput.hrtfFilePath = "";
            if (Application.isPlaying && masterOutput.isInitialized)
                masterOutput.LoadDefaultHRTF();
            shouldSave = true;
        }
        GUILayout.FlexibleSpace();
        EditorGUILayout.EndHorizontal();

        GUILayout.Label("Sélectionne un fichier .txt HRTF dans StreamingAssets/HRTF. " +
                        "Le chemin est stocké relatif à StreamingAssets (portable Mac/Windows).",
            EditorStyles.wordWrappedMiniLabel);

        // ── HRTF truncation ───────────────────────────────────────────────
        GUILayout.Space(6);
        EditorGUILayout.BeginHorizontal();
        GUILayout.Label("Limit HRTF to 512 samples", GUILayout.Width(200));
        bool newTrunc = EditorGUILayout.Toggle(outputState.isHrtfTruncated, GUILayout.Width(20));
        if (newTrunc != outputState.isHrtfTruncated)
        {
            outputState.isHrtfTruncated  = newTrunc;
            masterOutput.isHrtfTruncated = newTrunc;
            shouldSave = true;
        }
        EditorGUILayout.EndHorizontal();
        GUILayout.Label(
            "Truncates each HRTF impulse response to 512 samples before convolution. " +
            "Reduces CPU load at high virtual-speaker counts (≥ 64) at the cost of minor low-frequency accuracy.",
            EditorStyles.wordWrappedMiniLabel);

        EditorGUILayout.EndVertical();
    }

    private void DrawCleanButtonSection()
    {
        GUILayout.Space(10);
        EditorGUILayout.BeginHorizontal();
        GUILayout.FlexibleSpace();
        if (GUILayout.Button("CLEAN STATE", GUILayout.Width(120), GUILayout.Height(30)))
            At_AudioEngineUtils.CleanAllStates(SceneManager.GetActiveScene().name);
        GUILayout.FlexibleSpace();
        EditorGUILayout.EndHorizontal();
    }

    private void UpdateMasterOutputParameters()
    {
        masterOutput.audioDeviceName          = outputState.audioDeviceName;
        masterOutput.outputChannelCount       = outputState.outputChannelCount;
        masterOutput.samplingRate             = outputState.samplingRate;
        masterOutput.isStartingEngineOnAwake  = outputState.isStartingEngineOnAwake;
        masterOutput.outputConfigDimension    = outputState.outputConfigDimension;
        masterOutput.virtualSpeakerRigSize    = outputState.virtualSpeakerRigSize;
        masterOutput.maxDistanceForDelay      = outputState.maxDistanceForDelay;
        masterOutput.isBinauralVirtualization = outputState.isBinauralVirtualization;
        masterOutput.isSimpleBinauralSpat     = outputState.isSimpleBinauralSpat;
        masterOutput.isNearFieldCorrection    = outputState.isNearFieldCorrection;
        masterOutput.hrtfDistance             = outputState.hrtfDistance;
        masterOutput.isHrtfTruncated          = outputState.isHrtfTruncated;
        masterOutput.numVirtualSpeakers       = outputState.numVirtualSpeakers;
    }

    private void RegenerateSpeakerConfiguration()
    {
        At_VirtualSpeaker[] vss = FindObjectsOfType<At_VirtualSpeaker>();
        GameObject parent = (vss != null && vss.Length > 0) ? vss[0].transform.parent.gameObject : null;
        foreach (At_VirtualSpeaker vs in vss) DestroyImmediate(vs.gameObject);
        if (parent != null) DestroyImmediate(parent);

        GameObject virtualSpkParent = new GameObject("VirtualSpeakers");
        virtualSpkParent.transform.parent = masterOutput.gameObject.transform;

        At_SpeakerConfig.addSpeakerConfigToScene(
            ref speakers,
            outputState.virtualSpeakerRigSize,
            outputState.numVirtualSpeakers,
            outputState.outputConfigDimension,
            virtualSpkParent);

        EditorSceneManager.SaveScene(SceneManager.GetActiveScene());
    }
    #endregion

    #region Metering Display
    private void DisplayMeteringWithWidth(float[] metering, bool isPlaying, int numChannels, float maxWidth, int meterHeight)
    {
        if (meterOff == null) return;

        float meterWidth = Mathf.Clamp(maxWidth / numChannels,
            0.5f, (128f / meterOff.height) * meterOff.width);
        float totalWidth = meterWidth * numChannels;

        Rect fullRect = GUILayoutUtility.GetRect(totalWidth, meterHeight,
            GUILayout.Width(totalWidth), GUILayout.Height(meterHeight));

        DrawMeterBars(metering, isPlaying, numChannels, meterWidth, fullRect);
    }

    private void DrawMeterBars(float[] metering, bool isPlaying, int numChannels, float meterWidth, Rect fullRect)
    {
        int[]   segmentPixels = { 0, 18, 38, 60, 89, 130, 187, 244, 300 };
        float[] segmentDB     = { -80f, -60f, -50f, -40f, -30f, -20f, -10f, 0f, 10f };

        for (int i = 0; i < numChannels; i++)
        {
            Rect meterRect = new Rect(fullRect.x + meterWidth * i, fullRect.y, meterWidth, fullRect.height);
            GUI.DrawTexture(meterRect, meterOff);

            float db = Mathf.Clamp(i < metering.Length ? metering[i] : -86f, -80f, 10f);

            int seg = 1;
            while (seg < segmentDB.Length && segmentDB[seg] < db) seg++;

            float visible = 0;
            if (seg < segmentDB.Length)
                visible = segmentPixels[seg - 1] +
                    ((db - segmentDB[seg - 1]) / (segmentDB[seg] - segmentDB[seg - 1])) *
                    (segmentPixels[seg] - segmentPixels[seg - 1]);

            visible *= fullRect.height / meterOff.height;

            if (isPlaying)
            {
                Rect pos = new Rect(meterRect.x, fullRect.height - visible + meterRect.y, meterWidth, visible);
                Rect uv  = new Rect(0, 0, 1f, visible / fullRect.height);
                GUI.DrawTextureWithTexCoords(pos, meterOn, uv);
            }
        }
    }

    public override bool RequiresConstantRepaint() =>
        masterOutput != null && masterOutput.isPlaying;

    // Helper methods for input validation (used by legacy GUI code)
    public static string CleanStringForFloat(string input) =>
        Regex.Match(input, @"^-?[0-9]*(?:\.[0-9]*)?$").Success ? input : "0";

    public static string CleanStringForInt(string input) =>
        Regex.Match(input, "([-+]?[0-9]+)").Success ? input : "0";

    private void HorizontalLine(Color color)
    {
        var c = GUI.color;
        GUI.color = color;
        GUILayout.Box(GUIContent.none, horizontalLine);
        GUI.color = c;
    }
    #endregion

    #region Deferred Actions
    /// <summary>
    /// Executes the spatconfig load dialog outside the IMGUI layout pass.
    /// Registered via EditorApplication.delayCall when the Load button is pressed.
    /// </summary>
    private void ExecutePendingSpatConfigLoad()
    {
        if (!m_pendingSpatConfigLoad || masterOutput == null) return;
        m_pendingSpatConfigLoad = false;
        ApplyCustomSpeakerConfiguration();
        Repaint();
    }

    /// <summary>
    /// Executes the spatconfig save dialog outside the IMGUI layout pass.
    /// Registered via EditorApplication.delayCall when the Save button is pressed.
    /// </summary>
    private void ExecutePendingSpatConfigSave()
    {
        if (!m_pendingSpatConfigSave || masterOutput == null) return;
        m_pendingSpatConfigSave = false;
        SaveCustomSpeakerConfiguration();
        Repaint();
    }

    /// <summary>
    /// Opens the HRTF file dialog and applies the result.
    /// Called via EditorApplication.delayCall to run outside the IMGUI layout pass —
    /// OpenFilePanel triggers ExitGUIException which aborts the pass before
    /// EndHorizontal and EndVertical are reached.
    /// </summary>
    private void ExecutePendingHRTFLoad()
    {
        if (!m_pendingHRTFLoad || masterOutput == null) return;
        m_pendingHRTFLoad = false;

        string hrtfDir = System.IO.Path.Combine(Application.streamingAssetsPath, "HRTF");
        if (!System.IO.Directory.Exists(hrtfDir))
            hrtfDir = Application.streamingAssetsPath;

        string absPath = EditorUtility.OpenFilePanel("Select HRTF Data File", hrtfDir, "txt");
        if (string.IsNullOrEmpty(absPath)) return;

        // Store path relative to StreamingAssets so it stays valid across machines / OS
        string streamingRoot = Application.streamingAssetsPath.Replace('\\', '/');
        string normalizedAbs = absPath.Replace('\\', '/');
        string relativePath  = normalizedAbs.StartsWith(streamingRoot)
            ? normalizedAbs.Substring(streamingRoot.Length).TrimStart('/')
            : absPath;   // fallback: keep absolute if selected outside StreamingAssets

        string fullPath = System.IO.Path.Combine(Application.streamingAssetsPath, relativePath);
        outputState.hrtfFilePath  = relativePath;
        masterOutput.hrtfFilePath = fullPath;

        if (Application.isPlaying && masterOutput.isInitialized)
        {
            if (!masterOutput.LoadHRTFFile(fullPath))
                Debug.LogError($"[AT_WS] Failed to load HRTF: {System.IO.Path.GetFileName(fullPath)}");
        }

        shouldSave = true;
        Repaint();
    }

    /// <summary>
    /// Switches the output to binaural virtualization mode and schedules a
    /// dialog notifying the user.  Must be called from within an IMGUI layout
    /// pass — the actual dialog is deferred via EditorApplication.delayCall to
    /// avoid ExitGUIException aborting the current pass.
    /// </summary>
    /// <param name="numVS">Requested virtual speaker count.</param>
    /// <param name="deviceMax">Physical output channel limit of the selected device.</param>
    private void SwitchToBinauralWithWarning(int numVS, int deviceMax)
    {
        outputState.isBinauralVirtualization  = true;
        masterOutput.isBinauralVirtualization = true;
        outputState.outputChannelCount        = 2;
        masterOutput.outputChannelCount       = 2;
        shouldSave = true;

        m_pendingBinauralAutoSwitchMessage =
            $"Number of Virtual Speakers ({numVS}) greater than number of output device " +
            $"channels ({deviceMax}).\n\n" +
            "Automatic switch to Binaural Virtualization for 2-channel downmix.";

        if (!m_pendingBinauralAutoSwitch)
        {
            m_pendingBinauralAutoSwitch = true;
            EditorApplication.delayCall += ExecutePendingBinauralAutoSwitch;
        }
    }

    /// <summary>
    /// Opens the binaural auto-switch notification dialog.
    /// Called via EditorApplication.delayCall to run outside the IMGUI layout pass.
    /// </summary>
    private void ExecutePendingBinauralAutoSwitch()
    {
        if (!m_pendingBinauralAutoSwitch) return;
        m_pendingBinauralAutoSwitch = false;
        EditorUtility.DisplayDialog("Binaural Virtualization Enabled",
            m_pendingBinauralAutoSwitchMessage, "OK");
        Repaint();
    }
    #endregion

    #region DLL Imports
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int  AT_WS_initialize();
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int  AT_WS_shutdown();
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int  AT_WS_getDeviceCount();
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int  AT_WS_getDeviceName(int deviceIndex, IntPtr buffer, int bufferSize);
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int  AT_WS_getDeviceChannels(int deviceIndex, int isOutput);
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int  AT_WS_isDeviceScanComplete();
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int  AT_WS_waitForDeviceScan(int timeoutMs);
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern void AT_WS_refreshDevices(int includeASIO, int async);
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int  AT_WS_getCachedDeviceInfo(int deviceIndex, IntPtr nameBuffer, IntPtr typeBuffer, ref int maxInputs, ref int maxOutputs);
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int  AT_WS_getDetailedDeviceInfo(int deviceIndex, IntPtr nameBuffer, IntPtr typeBuffer, ref int maxInputs, ref int maxOutputs);
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern void AT_WS_filterUnavailableDevices();
    #endregion
}
