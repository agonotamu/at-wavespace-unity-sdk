/// @file At_MasterOutput.cs
/// @brief Main audio output manager for the AT SPAT spatialization engine.
///
/// @details
/// Owns the engine lifecycle (initialize / setup / shutdown), the virtual speaker
/// rig, and the master gain/metering. Players register themselves through addPlayer().
/// Speaker positions are sent to the native library every frame using pre-allocated
/// arrays (sized to MAX_VIRTUAL_SPEAKERS) to avoid per-frame heap allocation.

using System.Collections.Generic;
using UnityEngine;
using System.Runtime.InteropServices;
using UnityEngine.SceneManagement;
using System;

enum FilterType { None = 0, LowPass = 1, HighPass = 2 }

public class At_MasterOutput : MonoBehaviour
{
    #region Constants
    private const int AUDIO_PLUGIN_OK    = 0;
    private const int AUDIO_PLUGIN_ERROR = 1;

    /// <summary>
    /// Maximum number of WFS channels. Must match MAX_VIRTUAL_SPEAKERS in the C++ library
    /// and At_Player.MAX_VIRTUAL_SPEAKERS.
    /// </summary>
    public const int MAX_VIRTUAL_SPEAKERS = 1024;
    #endregion

    #region Public Variables
    public List<At_Player> playerList;
    public At_VirtualSpeaker[] virtualSpeakers;

    public string audioDeviceName = "";
    public int    outputChannelCount;
    public int    outputConfigDimension;
    public float  gain;
    public float  makeupGain;
    public int    bufferSize = 512;
    public int    samplingRate;
    public bool   isStartingEngineOnAwake;
    public float  virtualSpeakerRigSize;
    public float  maxDistanceForDelay;
    public bool   isBinauralVirtualization;
    public bool   isSimpleBinauralSpat;
    public bool   isPrevSimpleBinauralSpat;
    public bool   isNearFieldCorrection;
    public bool   isPrevIsNearFieldCorrection;
    public float  hrtfDistance;
    public float  prevHrtfDistance = -1f;  // -1 forces send on first Update
    public string hrtfFilePath = "";
    public bool   isHrtfTruncated;
    public int    numVirtualSpeakers = 2;

    public bool    isPlaying     = false;
    public bool    isInitialized = false;

    /// <summary>
    /// Effective source radius (metres) for WFS singularity regularisation.
    /// Controls both audio (AT_WS_setSecondarySourceSize) and visual shader (_secondarySourceSize).
    /// P1: prevents cos(φ)/sqrt(r) amplitude divergence near the array plane.
    /// P2: replaces the hard speaker-mask gate with a raised-cosine taper.
    /// 0 = point source (original behaviour). Typical range: 0.05–0.5 m.
    /// </summary>
    [Range(0f, 1f)]
    public float secondarySourceSize = 0.3f;

    // ── Cached global player settings (sourced from outputState) ──────────────
    // Changed values are sent to the C++ engine once per Update() via the global
    // AT_WS_* functions. Players themselves no longer call these per-frame.
    private bool  m_prevIsWfsSpeakerMask       = true;
    private bool  m_prevIsPrefilter             = false;
    private bool  m_prevIsWfsGain               = false;
    private bool  m_prevIsActiveSpeakersMinMax  = false;
    private bool  m_prevIsHrtfTruncated         = false;
    private float m_prevSecondarySourceSize       = -1f;   // -1 forces send on first Update

    /// <summary>RMS meter values for each output channel (dB).</summary>
    public float[] meters;
    #endregion

    #region Private Variables
    private At_OutputState outputState;
    private int maxDeviceChannel;

    // Pre-allocated transform arrays — avoids per-frame GC pressure.
    // Must stay sized to MAX_VIRTUAL_SPEAKERS.
    private readonly float[] m_speakerPositions = new float[MAX_VIRTUAL_SPEAKERS * 3];
    private readonly float[] m_speakerRotations = new float[MAX_VIRTUAL_SPEAKERS * 3];
    private readonly float[] m_speakerForwards  = new float[MAX_VIRTUAL_SPEAKERS * 3];
    #endregion

    #region Log Callback
    private delegate void LogCallback(string message);

    [DllImport("at_wavespace_engine")]
    private static extern void AT_WS_setLogCallback(LogCallback callback);

    [AOT.MonoPInvokeCallback(typeof(LogCallback))]
    private static void OnSpatEngineLog(string message)
    {
        if      (message.Contains("[SPAT ERROR]"))   Debug.LogError(message);
        else if (message.Contains("[SPAT WARNING]")) Debug.LogWarning(message);
        else                                          Debug.Log(message);
    }
    #endregion

    #region Unity Lifecycle
    private void Awake()
    {
        AT_WS_setLogCallback(OnSpatEngineLog);

        At_Player[] players = FindObjectsOfType<At_Player>();

        outputState = At_AudioEngineUtils.getOutputState(SceneManager.GetActiveScene().name);

        audioDeviceName          = outputState.audioDeviceName;
        outputChannelCount       = outputState.outputChannelCount;
        outputConfigDimension    = outputState.outputConfigDimension;
        gain                     = outputState.gain;
        makeupGain               = outputState.makeupGain;
        samplingRate             = outputState.samplingRate;
        bufferSize               = outputState.bufferSize;
        isStartingEngineOnAwake  = outputState.isStartingEngineOnAwake;
        virtualSpeakerRigSize    = outputState.virtualSpeakerRigSize;
        maxDistanceForDelay      = outputState.maxDistanceForDelay;
        isBinauralVirtualization = outputState.isBinauralVirtualization;
        isSimpleBinauralSpat     = outputState.isSimpleBinauralSpat;
        isNearFieldCorrection    = outputState.isNearFieldCorrection;
        hrtfDistance             = outputState.hrtfDistance;
        numVirtualSpeakers       = outputState.numVirtualSpeakers;
        hrtfFilePath             = string.IsNullOrEmpty(outputState.hrtfFilePath)
            ? ""
            : System.IO.Path.Combine(Application.streamingAssetsPath, outputState.hrtfFilePath);
        isHrtfTruncated          = outputState.isHrtfTruncated;
        secondarySourceSize        = outputState.secondarySourceSize;

        // Prime prev-values so first Update() sends the correct flags to the engine.
        m_prevIsWfsSpeakerMask      = !outputState.isWfsSpeakerMask;      // force send
        m_prevIsPrefilter           = !outputState.isPrefilter;
        m_prevIsWfsGain             = !outputState.isWfsGain;
        m_prevIsActiveSpeakersMinMax = !outputState.isActiveSpeakersMinMax;
        m_prevIsHrtfTruncated       = !outputState.isHrtfTruncated;       // force send

        meters = new float[outputChannelCount];

        virtualSpeakers = FindObjectsOfType<At_VirtualSpeaker>();
        foreach (At_VirtualSpeaker vs in virtualSpeakers)
            vs.m_maxDistanceForDelay = outputState.maxDistanceForDelay;

        InitSpatializerEngine();

        foreach (At_Player p in players)
        {
            if (!p.isInitialized)
            {
                addPlayer(p);
                p.initPlayer();
            }
        }

        UpdateVirtualSpeakerPosition();
        isPlaying = true;

        foreach (SoundWaveShaderManager swsm in FindObjectsOfType<SoundWaveShaderManager>())
            swsm.Init();
    }

    private void OnDisable()
    {
        try { AT_WS_setLogCallback(null); } catch { }
        Shutdown();
    }

    public void OnApplicationQuit()
    {
        Shutdown();
        At_AudioEngineUtils.CleanAllStates(SceneManager.GetActiveScene().name);
    }

    private void OnSceneUnloaded(Scene current)
    {
        Shutdown();
    }

    private void Update()
    {
        if (!isInitialized) return;

        if (AT_WS_setMasterGain(gain) == AUDIO_PLUGIN_ERROR)
            Debug.LogError("[AudioPlugin] Cannot update master gain");

        if (AT_WS_setMakeupMasterGain(makeupGain) == AUDIO_PLUGIN_ERROR)
            Debug.LogError("[AudioPlugin] Cannot update master makeup gain");

        UpdateVirtualSpeakerPosition();

        if (isSimpleBinauralSpat != isPrevSimpleBinauralSpat)
        {
            if (AT_WS_setIsSimpleBinauralSpat(isSimpleBinauralSpat) == AUDIO_PLUGIN_ERROR)
                Debug.LogError("[AudioPlugin] Failed to set simple binaural to " + isSimpleBinauralSpat);
            isPrevSimpleBinauralSpat = isSimpleBinauralSpat;
        }

        if (isNearFieldCorrection != isPrevIsNearFieldCorrection)
        {
            if (AT_WS_setIsNearFieldCorrection(isNearFieldCorrection) == AUDIO_PLUGIN_ERROR)
                Debug.LogError("[AudioPlugin] Failed to set near-field correction to " + isNearFieldCorrection);
            isPrevIsNearFieldCorrection = isNearFieldCorrection;
        }

        if (hrtfDistance != prevHrtfDistance)
        {
            if (AT_WS_setNearFieldCorrectionRRef(hrtfDistance, 0f) == AUDIO_PLUGIN_ERROR)
                Debug.LogError("[AudioPlugin] Failed to set NFC rRef to " + hrtfDistance);
            prevHrtfDistance = hrtfDistance;
        }

        // ── Global player settings — send only when value changes ────────────────
        if (outputState.isWfsSpeakerMask != m_prevIsWfsSpeakerMask)
        {
            AT_WS_enableAllPlayersSpeakerMask(outputState.isWfsSpeakerMask);
            m_prevIsWfsSpeakerMask = outputState.isWfsSpeakerMask;
        }
        if (outputState.isPrefilter != m_prevIsPrefilter)
        {
            AT_WS_setIsPrefilterAllPlayers(outputState.isPrefilter);
            m_prevIsPrefilter = outputState.isPrefilter;
        }
        if (outputState.isWfsGain != m_prevIsWfsGain)
        {
            AT_WS_setIsWfsGain(outputState.isWfsGain);
            m_prevIsWfsGain = outputState.isWfsGain;
        }
        if (outputState.isActiveSpeakersMinMax != m_prevIsActiveSpeakersMinMax)
        {
            AT_WS_setIsActiveSpeakersMinMax(outputState.isActiveSpeakersMinMax);
            m_prevIsActiveSpeakersMinMax = outputState.isActiveSpeakersMinMax;
        }

        if (isHrtfTruncated != m_prevIsHrtfTruncated)
        {
            if (AT_WS_setHrtfTruncate(isHrtfTruncated) == AUDIO_PLUGIN_ERROR)
                Debug.LogError("[AudioPlugin] Failed to set HRTF truncation to " + isHrtfTruncated);
            outputState.isHrtfTruncated  = isHrtfTruncated;
            m_prevIsHrtfTruncated        = isHrtfTruncated;
        }

        // ── WFS source regularisation (P1 + P2) — send only on change ────────────
        if (!Mathf.Approximately(secondarySourceSize, m_prevSecondarySourceSize))
        {
            outputState.secondarySourceSize = secondarySourceSize;

            if (AT_WS_setSecondarySourceSize(secondarySourceSize) == AUDIO_PLUGIN_ERROR)
                Debug.LogError("[AudioPlugin] Failed to set source size to " + secondarySourceSize);

            // Keep visual shader coherent with the audio engine
            foreach (SoundWaveShaderManager swsm in FindObjectsOfType<SoundWaveShaderManager>())
                swsm.SetSecondarySourceSize(secondarySourceSize);

            m_prevSecondarySourceSize = secondarySourceSize;
        }

        getMeters(meters, outputChannelCount);
    }
    #endregion

    #region Engine Initialization
    private void InitSpatializerEngine()
    {
        if (AT_WS_initialize() != AUDIO_PLUGIN_OK)
        {
            Debug.LogError("[AudioPlugin] Failed to initialize plugin");
            return;
        }

        int result = AT_WS_setup(audioDeviceName, 0, numVirtualSpeakers, bufferSize, isBinauralVirtualization);

        if (isBinauralVirtualization)
        {
            if (!string.IsNullOrEmpty(hrtfFilePath) && System.IO.File.Exists(hrtfFilePath))
                LoadHRTFFile(hrtfFilePath);
            else
                LoadDefaultHRTF();

            if (isSimpleBinauralSpat)
            {
                if (AT_WS_setIsSimpleBinauralSpat(true) == AUDIO_PLUGIN_OK)
                    Debug.Log("[AudioPlugin] Simple binaural mode enabled");
                else
                    Debug.LogError("[AudioPlugin] Failed to enable simple binaural mode");
            }
        }

        if (result != AUDIO_PLUGIN_OK)
        {
            Debug.LogError("[AudioPlugin] Failed to setup audio device");
            AT_WS_shutdown();
            return;
        }

        isInitialized = true;
    }

    private void Shutdown()
    {
        if (!isInitialized) return;

        if (AT_WS_stopAllPlayers() != AUDIO_PLUGIN_OK)
            Debug.LogError("[AudioPlugin] Failed to stop playback");
        else
            isPlaying = false;

        playerList.Clear();

        if (AT_WS_shutdown() != AUDIO_PLUGIN_OK)
            Debug.LogError("[AudioPlugin] Failed to shutdown cleanly");
        else
            isInitialized = false;
    }
    #endregion

    #region Player Management
    /// <summary>
    /// Registers an At_Player with the native engine and assigns it a spatial ID.
    /// </summary>
    /// <returns>0 on success, -1 on failure or duplicate.</returns>
    public int addPlayer(At_Player p)
    {
        if (!isInitialized)           { Debug.LogError("[AudioPlugin] Not initialized"); return -1; }
        if (playerList == null)       playerList = new List<At_Player>();

        foreach (At_Player existing in playerList)
            if (existing.guid == p.guid) return -1;

        int uid;
        if (AT_WS_addPlayer(out uid, p.is3D, p.isLooping) != AUDIO_PLUGIN_OK)
        {
            Debug.LogError("[AudioPlugin] Failed to create player");
            return -1;
        }

        p.spatID             = uid;
        p.masterOutput       = this;
        p.outputChannelCount = outputChannelCount;
        playerList.Add(p);
        return 0;
    }

    /// <summary>Removes the player with the given spatial ID from the managed list.</summary>
    public void removePlayerFromListWithSpatID(int spatID)
    {
        for (int i = 0; i < playerList.Count; i++)
        {
            if (playerList[i].spatID == spatID)
            {
                playerList.RemoveAt(i);
                return;
            }
        }
    }
    #endregion

    #region Virtual Speaker Management
    /// <summary>
    /// Sends current virtual speaker transforms to the native library.
    /// Uses pre-allocated arrays (indexed by speaker ID) to avoid heap allocation.
    /// </summary>
    private void UpdateVirtualSpeakerPosition()
    {
        for (int i = 0; i < virtualSpeakers.Length; i++)
        {
            Transform t = virtualSpeakers[i].gameObject.transform;

            float eulerX = t.eulerAngles.x;
            float eulerY = t.eulerAngles.y;
            float eulerZ = t.eulerAngles.z;

            // Normalize gimbal-lock edge case (Y=180, Z=180)
            if (eulerY == 180 && eulerZ == 180) { eulerX = 180 - eulerX; eulerY = 0; eulerZ = 0; }

            int b = virtualSpeakers[i].id * 3;
            m_speakerPositions[b]     = t.position.x;
            m_speakerPositions[b + 1] = t.position.y;
            m_speakerPositions[b + 2] = t.position.z;
            m_speakerRotations[b]     = eulerX;
            m_speakerRotations[b + 1] = eulerY;
            m_speakerRotations[b + 2] = eulerZ;
            m_speakerForwards[b]      = t.forward.x;
            m_speakerForwards[b + 1]  = t.forward.y;
            m_speakerForwards[b + 2]  = t.forward.z;
        }

        if (AT_WS_setVirtualSpeakerTransform(m_speakerPositions, m_speakerRotations, m_speakerForwards, numVirtualSpeakers) == AUDIO_PLUGIN_ERROR)
            Debug.LogError("[AudioPlugin] Cannot update virtual speaker transforms");
    }

    /// <summary>Returns the virtual speaker with the given index, or null.</summary>
    public At_VirtualSpeaker speakerWithIndex(int index)
    {
        if (virtualSpeakers != null)
            foreach (At_VirtualSpeaker vs in virtualSpeakers)
                if (vs.id == index) return vs;
        return null;
    }
    #endregion

    #region HRTF Management
    /// <summary>Loads an HRTF file from disk into the native library.</summary>
    /// <param name="filePath">Absolute path to the .txt HRTF data file.</param>
    /// <returns>True on success.</returns>
    public bool LoadHRTFFile(string filePath)
    {
        if (string.IsNullOrEmpty(filePath))   { Debug.LogError("[AT_WS] HRTF file path is empty");           return false; }
        if (!System.IO.File.Exists(filePath)) { Debug.LogError("[AT_WS] HRTF file not found: " + filePath);  return false; }

        if (AT_WS_loadHRTF(filePath) == AUDIO_PLUGIN_OK)
        {
            hrtfFilePath = filePath;
            Debug.Log("[AT_WS] HRTF loaded: " + filePath);
            return true;
        }

        Debug.LogError("[AT_WS] Failed to load HRTF: " + filePath);
        return false;
    }

    /// <summary>Loads the built-in default HRTF.</summary>
    public void LoadDefaultHRTF()
    {
        if (AT_WS_loadDefaultHRTF() == AUDIO_PLUGIN_OK)
        {
            hrtfFilePath = "[Default]";
            Debug.Log("[AT_WS] Default HRTF loaded");
        }
        else
        {
            Debug.LogError("[AT_WS] Failed to load default HRTF");
        }
    }
    #endregion

    #region Metering
    /// <summary>Reads output RMS meter values from the native library into the provided array.</summary>
    public unsafe void getMeters(float[] meters, int arraySize)
    {
        fixed (float* ptr = meters)
        {
            if (AT_WS_getMixerOutputMeters((IntPtr)ptr, arraySize) == AUDIO_PLUGIN_ERROR)
                Debug.LogError("[AudioPlugin] Cannot get mixer output meters");
        }
    }
    #endregion

    #region Gizmos
#if UNITY_EDITOR
    private void OnDrawGizmos()
    {
        At_VirtualSpeaker[] vss = FindObjectsOfType<At_VirtualSpeaker>();
        if (vss == null || vss.Length == 0) return;

        // Draw lines between adjacent speakers to visualize the rig geometry
        for (int i = 0; i < vss.Length; i++)
        {
            At_VirtualSpeaker a = speakerWithIndex(vss, i);
            At_VirtualSpeaker b = speakerWithIndex(vss, (i + 1) % vss.Length);
            if (a != null && b != null)
            {
                Gizmos.color = new Color(0f, 1f, 1f, 1f);
                Gizmos.DrawLine(a.gameObject.transform.position, b.gameObject.transform.position);
            }
        }
    }

    private At_VirtualSpeaker speakerWithIndex(At_VirtualSpeaker[] vss, int index)
    {
        foreach (At_VirtualSpeaker vs in vss)
            if (vs.id == index) return vs;
        return null;
    }
#endif
    #endregion

    #region DLL Imports
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int AT_WS_initialize();
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int AT_WS_shutdown();
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int AT_WS_getDeviceCount();
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int AT_WS_getDeviceName(int deviceIndex, IntPtr buffer, int bufferSize);
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int AT_WS_getDeviceChannels(int deviceIndex, int isOutput);
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int AT_WS_setup(string deviceName, int inputChannels, int outputChannels, int bufferSize, bool isBinauralVirtualization);
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int AT_WS_addPlayer(out int uid, bool is3D, bool isLooping);
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int AT_WS_removePlayer(int uid);
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int AT_WS_startPlayer(int uid);
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int AT_WS_stopPlayer(int uid);
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int AT_WS_stopAllPlayers();
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int AT_WS_setMasterGain(float masterGain);
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int AT_WS_setMakeupMasterGain(float makeupMasterGain);
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int AT_WS_setVirtualSpeakerTransform(float[] positions, float[] rotations, float[] forwards, int count);
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int AT_WS_getMixerOutputMeters(IntPtr meters, int arraySize);
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int AT_WS_setIsBinauralVirtualization(bool isBinauralVirtualization);
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall, CharSet = CharSet.Ansi)] private static extern int AT_WS_loadHRTF(string filePath);
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int AT_WS_loadDefaultHRTF();
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int AT_WS_setIsSimpleBinauralSpat(bool isSimpleBinauralSpat);
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int AT_WS_setIsNearFieldCorrection(bool isNearFieldCorrection);
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int AT_WS_setNearFieldCorrectionRRef(float rRef, float headRadius);
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int AT_WS_enableAllPlayersSpeakerMask(bool isWfsSpeakerMask);
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int AT_WS_setIsPrefilterAllPlayers(bool isPrefilter);
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int AT_WS_setIsWfsGain(bool isWfsGain);
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int AT_WS_setIsActiveSpeakersMinMax(bool enabled);
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int AT_WS_setSecondarySourceSize(float sourceSize);
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int AT_WS_setHrtfTruncate(bool enabled);
    #endregion
}
