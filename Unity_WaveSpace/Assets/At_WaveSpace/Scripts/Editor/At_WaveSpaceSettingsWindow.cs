/// @file At_WaveSpaceSettingsWindow.cs
/// @brief AT_WaveSpace global settings window.
///
/// @details
/// All settings that apply to the engine or to ALL players:
///   MASTER  : Simple Binaural A/B, Near-Field Correction, HRTF Distance
///   PLAYERS : WFS Output Mask, Pre-filtering, WFS Gain,
///             Active-speakers min/max for focused sources
///
/// Open via: AT_WaveSpace → Settings…

using UnityEngine;
using UnityEditor;
using UnityEngine.SceneManagement;
using System.Runtime.InteropServices;

public class At_WaveSpaceSettingsWindow : EditorWindow
{
    // =========================================================================
    private const int AUDIO_PLUGIN_OK = 0;

    private At_MasterOutput masterOutput;
    private At_OutputState  outputState;
    private GUIStyle        m_headerStyle;
    private GUIStyle        m_hLine;
    private bool            m_stylesBuilt = false;
    private bool            m_shouldSave  = false;
    private Vector2         m_scrollPos;

    // =========================================================================
    [MenuItem("AT_WaveSpace/Advanced Settings\u2026")]
    public static void ShowWindow()
    {
        var w = GetWindow<At_WaveSpaceSettingsWindow>("AT WaveSpace — Settings");
        w.minSize = new Vector2(380, 500);
        w.Show();
    }

    // =========================================================================
    private void OnEnable()
    {
        Refresh();
        EditorApplication.playModeStateChanged += OnPlayModeChanged;
    }

    private void OnDisable()
    {
        EditorApplication.playModeStateChanged -= OnPlayModeChanged;
        Save();
    }

    private void OnPlayModeChanged(PlayModeStateChange s)
    {
        if (s == PlayModeStateChange.EnteredPlayMode ||
            s == PlayModeStateChange.EnteredEditMode)
            Refresh();
    }

    private void Refresh()
    {
        masterOutput = FindObjectOfType<At_MasterOutput>();
        outputState  = At_AudioEngineUtils.getOutputState(
                           SceneManager.GetActiveScene().name);
    }

    private void Save()
    {
        if (!m_shouldSave) return;
        At_AudioEngineUtils.SaveAllState(SceneManager.GetActiveScene().name);
        m_shouldSave = false;
    }

    // =========================================================================
    private void OnGUI()
    {
        BuildStyles();

        if (masterOutput == null || outputState == null)
        {
            Refresh();
            if (masterOutput == null)
            {
                EditorGUILayout.HelpBox(
                    "No At_MasterOutput found in the scene.",
                    MessageType.Warning);
                return;
            }
        }

        m_scrollPos = EditorGUILayout.BeginScrollView(m_scrollPos);

        EditorGUILayout.Space(8);
        GUILayout.Label("AT WaveSpace — Global Settings", m_headerStyle);
        EditorGUILayout.Space(6);

        DrawMasterSection();
        EditorGUILayout.Space(10);
        DrawPlayersSection();
        EditorGUILayout.Space(14);

        EditorGUILayout.BeginHorizontal();
        GUILayout.FlexibleSpace();
        if (GUILayout.Button("Save", GUILayout.Width(90), GUILayout.Height(26)))
            Save();
        GUILayout.Space(8);
        if (GUILayout.Button("Revert", GUILayout.Width(90), GUILayout.Height(26)))
        { Refresh(); m_shouldSave = false; }
        GUILayout.FlexibleSpace();
        EditorGUILayout.EndHorizontal();

        EditorGUILayout.EndScrollView();
    }

    // ── MASTER section ────────────────────────────────────────────────────────
    private void DrawMasterSection()
    {
        GUILayout.Label("MASTER", m_headerStyle);
        DrawHLine();

        bool hasBinaural = outputState.isBinauralVirtualization;

        using (new EditorGUI.DisabledScope(!hasBinaural))
        {
            // Simple Binaural A/B
            bool newSB = ToggleRow("Simple Binaural Mode (A/B Test)",
                                    outputState.isSimpleBinauralSpat);
            if (newSB != outputState.isSimpleBinauralSpat)
            {
                outputState.isSimpleBinauralSpat  = newSB;
                masterOutput.isSimpleBinauralSpat = newSB;
                if (Application.isPlaying)
                    AT_WS_setIsSimpleBinauralSpat(newSB);
                m_shouldSave = true;
            }
            EditorGUILayout.HelpBox(
                "Bypasses WFS synthesis; applies HRTF directly to each source (A/B comparison).",
                MessageType.None);

            EditorGUILayout.Space(6);

            // Near-Field Correction
            bool newNfc = ToggleRow("Near-Field Correction (NFC)",
                                     outputState.isNearFieldCorrection);
            if (newNfc != outputState.isNearFieldCorrection)
            {
                outputState.isNearFieldCorrection  = newNfc;
                masterOutput.isNearFieldCorrection = newNfc;
                if (Application.isPlaying)
                    AT_WS_setIsNearFieldCorrection(newNfc);
                m_shouldSave = true;
            }

            // HRTF Distance
            EditorGUILayout.BeginHorizontal();
            GUILayout.Label("HRTF Distance (m)", GUILayout.Width(240));
            string ds = EditorGUILayout.TextField(
                outputState.hrtfDistance.ToString("F2"), GUILayout.Width(60));
            GUILayout.Label("m", GUILayout.Width(14));
            EditorGUILayout.EndHorizontal();

            if (float.TryParse(ds,
                    System.Globalization.NumberStyles.Float,
                    System.Globalization.CultureInfo.InvariantCulture,
                    out float nd)
                && nd > 0f && !Mathf.Approximately(nd, outputState.hrtfDistance))
            {
                outputState.hrtfDistance  = nd;
                masterOutput.hrtfDistance = nd;
                if (Application.isPlaying)
                    AT_WS_setNearFieldCorrectionRRef(nd, 0f);
                m_shouldSave = true;
            }

            EditorGUILayout.HelpBox(
                "NFC applies a rigid-sphere ILD correction (Duda & Martens 1998). " +
                "HRTF Distance is the measurement distance of the BRIR set (typically 1.0 m).",
                MessageType.None);
        }

        if (!hasBinaural)
            EditorGUILayout.HelpBox(
                "Enable Binaural Virtualization in At_MasterOutput to use these settings.",
                MessageType.Info);

        EditorGUILayout.Space(6);
        DrawHLine();
        EditorGUILayout.Space(4);

        // ── Wavefront Visualisation ───────────────────────────────────────────
        GUILayout.Label("Wavefront Visualisation", m_headerStyle);

        EditorGUILayout.HelpBox(
            "Adds the WavefrontDisplay prefab to the scene at the position of the " +
            "At_MasterOutput object. The prefab contains the compute shader and " +
            "material required to visualise WFS wavefronts in real time.",
            MessageType.None);

        EditorGUILayout.BeginHorizontal();
        GUILayout.FlexibleSpace();
        if (GUILayout.Button("Add Wavefront Display",
                             GUILayout.Width(220), GUILayout.Height(24)))
            AddWavefrontDisplayPrefab();
        GUILayout.FlexibleSpace();
        EditorGUILayout.EndHorizontal();

        GUILayout.Space(4);

        bool displayExists = (GameObject.Find("WavefrontDisplay") != null);
        using (new EditorGUI.DisabledScope(!displayExists))
        {
            EditorGUILayout.BeginHorizontal();
            GUILayout.FlexibleSpace();
            if (GUILayout.Button("Remove Wavefront Display",
                                 GUILayout.Width(220), GUILayout.Height(24)))
                RemoveWavefrontDisplay();
            GUILayout.FlexibleSpace();
            EditorGUILayout.EndHorizontal();
        }
    }

    // ── PLAYERS section ───────────────────────────────────────────────────────
    private void DrawPlayersSection()
    {
        GUILayout.Label("PLAYERS  (applied to all players)", m_headerStyle);
        DrawHLine();

        // WFS Output Mask
        bool newMask = ToggleRow("Enable WFS Output Mask",
                                  outputState.isWfsSpeakerMask);
        if (newMask != outputState.isWfsSpeakerMask)
        {
            outputState.isWfsSpeakerMask = newMask;
            if (Application.isPlaying)
                AT_WS_enableAllPlayersSpeakerMask(newMask);
            m_shouldSave = true;
        }
        EditorGUILayout.HelpBox(
            "Activates only speakers on the correct side of the perpendicular to the " +
            "listener-source line through the virtual source.",
            MessageType.None);

        EditorGUILayout.Space(6);

        // Pre-filtering
        bool newPre = ToggleRow("Pre-filtering  (2.5D \u221a(j\u03c9) correction)",
                                 outputState.isPrefilter);
        if (newPre != outputState.isPrefilter)
        {
            outputState.isPrefilter = newPre;
            if (Application.isPlaying)
                AT_WS_setIsPrefilterAllPlayers(newPre);
            m_shouldSave = true;
        }
        EditorGUILayout.HelpBox(
            "Applies a half-derivative IIR prefilter to correct the +3 dB/oct amplitude " +
            "error inherent in 2.5D WFS synthesis.",
            MessageType.None);

        EditorGUILayout.Space(6);

        // WFS Gain
        bool newGain = ToggleRow("WFS Gain  cos(\u03c6) / \u221ar",
                                  outputState.isWfsGain);
        if (newGain != outputState.isWfsGain)
        {
            outputState.isWfsGain = newGain;
            if (Application.isPlaying)
                AT_WS_setIsWfsGain(newGain);
            m_shouldSave = true;
        }
        EditorGUILayout.HelpBox(
            "Applies per-speaker 2.5D amplitude weighting: gain = cos(\u03c6)/\u221ar. " +
            "Disable for delay-only WFS.",
            MessageType.None);

        EditorGUILayout.Space(6);
        DrawHLine();
        EditorGUILayout.Space(4);

        // Active-speakers min/max
        bool newAsm = ToggleRow("Active-speakers min/max  (focused sources)",
                                 outputState.isActiveSpeakersMinMax);
        if (newAsm != outputState.isActiveSpeakersMinMax)
        {
            outputState.isActiveSpeakersMinMax = newAsm;
            if (Application.isPlaying)
                AT_WS_setIsActiveSpeakersMinMax(newAsm);
            m_shouldSave = true;
        }
        EditorGUILayout.HelpBox(
            "Restricts the focused-source time-reversal reference (min/max delay) to " +
            "active speakers only. Prevents L/R inversion when the WFS Output Mask is active.",
            MessageType.None);

        EditorGUILayout.Space(6);
        DrawHLine();
        EditorGUILayout.Space(4);

        // ── Source Size (P1 + P2) ─────────────────────────────────────────────
        GUILayout.Label("WFS Source Regularisation", EditorStyles.boldLabel);

        EditorGUILayout.BeginHorizontal();
        GUILayout.Label(
            new GUIContent(
                "Source Size (m)",
                "Effective source radius for WFS singularity regularisation.\n\n" +
                "P1 — Amplitude: r_eff = sqrt(r² + ε²) prevents cos(φ)/sqrt(r) from " +
                "diverging when the source crosses the array plane. " +
                "Delay uses r_raw so wavefront phase is preserved.\n\n" +
                "P2 — Mask taper: replaces the hard speaker-activation gate with a " +
                "C¹ raised-cosine ramp over ±ε, eliminating the silence step at " +
                "the non-focused ↔ focused transition.\n\n" +
                "0 = ideal point source (original behaviour).\n" +
                "Same value forwarded to the wavefront display shader."),
            GUILayout.Width(160));

        float newSS = EditorGUILayout.Slider(
            outputState.secondarySourceSize, 0f, 1f, GUILayout.Width(200));
        GUILayout.Label("m", GUILayout.Width(14));
        EditorGUILayout.EndHorizontal();

        if (!Mathf.Approximately(newSS, outputState.secondarySourceSize))
        {
            outputState.secondarySourceSize      = newSS;
            masterOutput.secondarySourceSize     = newSS;
            if (Application.isPlaying)
                AT_WS_setSecondarySourceSize(newSS);
            m_shouldSave = true;
        }
        EditorGUILayout.HelpBox(
            "Shared between the audio engine and the WaveSpace wavefront shader. " +
            "Increase to smooth the array-plane transition; 0.3 m is a good default " +
            "for a 32-speaker / 5.7 m array.",
            MessageType.None);
    }

    // =========================================================================
    private bool ToggleRow(string label, bool value)
    {
        EditorGUILayout.BeginHorizontal();
        GUILayout.Label(label, GUILayout.Width(260));
        bool result = EditorGUILayout.Toggle(value, GUILayout.Width(20));
        EditorGUILayout.EndHorizontal();
        return result;
    }

    // ── Wavefront Display instantiation ───────────────────────────────────────
    private const string WAVEFRONT_PREFAB_RESOURCE = "At_WaveSpace/WavefrontsVisualisation/WavefrontDisplay";

    private void RemoveWavefrontDisplay()
    {
        GameObject display = GameObject.Find("WavefrontDisplay");
        if (display == null)
        {
            EditorUtility.DisplayDialog(
                "Not found",
                "No 'WavefrontDisplay' object found in the scene.",
                "OK");
            return;
        }

        Undo.DestroyObjectImmediate(display);
        EditorGUIUtility.ExitGUI(); // évite un repaint sur un objet détruit
    }

    private void AddWavefrontDisplayPrefab()
    {
        // Guard : ne pas ajouter si un WavefrontDisplay est déjà dans la scène
        if (GameObject.Find("WavefrontDisplay") != null)
        {
            EditorUtility.DisplayDialog(
                "Already in scene",
                "A \'WavefrontDisplay\' object is already present in the scene.\n\n" +
                "Use \'Remove Wavefront Display\' first if you want to replace it.",
                "OK");
            return;
        }

        // Load the prefab asset from Resources
        GameObject prefab = Resources.Load<GameObject>(WAVEFRONT_PREFAB_RESOURCE);
        if (prefab == null)
        {
            EditorUtility.DisplayDialog(
                "Prefab not found",
                "Could not find resource:\n" + WAVEFRONT_PREFAB_RESOURCE +
                "\n\nMake sure 'WavefrontDisplay.prefab' is inside a 'Resources/At_WaveSpace/' folder.",
                "OK");
            return;
        }

        // Determine spawn position from the At_MasterOutput GameObject
        Vector3 spawnPosition = Vector3.zero;
        if (masterOutput != null)
            spawnPosition = masterOutput.gameObject.transform.position;

        // Instantiate as a proper scene object with full Undo support
        GameObject instance = (GameObject)PrefabUtility.InstantiatePrefab(prefab);
        instance.transform.position = spawnPosition;

        // ── Scale the plane to match the Speaker Rig Size ─────────────────────
        // A Unity default plane is 10 × 10 world units at localScale (1,1,1).
        // SoundWaveShaderManager passes  _displayPlaneSizeX = 10 * localScale.x
        // to the compute shader, so:  localScale = rigSize / 10
        // The Y scale is kept at 1 (the plane is flat — Y has no visual effect).
        float rigSize = (outputState != null) ? outputState.virtualSpeakerRigSize : 1f;
        float planeScale = rigSize / 10f;
        instance.transform.localScale = new Vector3(planeScale, 1f, planeScale);

        Undo.RegisterCreatedObjectUndo(instance, "Add WavefrontDisplay");

        // Select the new object in the hierarchy so the user sees it immediately
        Selection.activeGameObject = instance;
        EditorGUIUtility.PingObject(instance);

    }

    private void BuildStyles()
    {
        if (m_stylesBuilt) return;
        m_headerStyle = new GUIStyle(EditorStyles.boldLabel) { fontSize = 11 };
        m_hLine = new GUIStyle
        {
            normal    = { background = EditorGUIUtility.whiteTexture },
            margin    = new RectOffset(0, 0, 3, 3),
            fixedHeight = 1
        };
        m_stylesBuilt = true;
    }

    private void DrawHLine()
    {
        var c = GUI.color; GUI.color = Color.black;
        GUILayout.Box(GUIContent.none, m_hLine);
        GUI.color = c;
    }

    // =========================================================================
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)]
    private static extern int AT_WS_setIsSimpleBinauralSpat(bool v);
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)]
    private static extern int AT_WS_setIsNearFieldCorrection(bool v);
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)]
    private static extern int AT_WS_setNearFieldCorrectionRRef(float rRef, float headRadius);
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)]
    private static extern int AT_WS_enableAllPlayersSpeakerMask(bool v);
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)]
    private static extern int AT_WS_setIsPrefilterAllPlayers(bool v);
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)]
    private static extern int AT_WS_setIsWfsGain(bool v);
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)]
    private static extern int AT_WS_setIsActiveSpeakersMinMax(bool v);
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)]
    private static extern int AT_WS_setSecondarySourceSize(float sourceSize);
}
