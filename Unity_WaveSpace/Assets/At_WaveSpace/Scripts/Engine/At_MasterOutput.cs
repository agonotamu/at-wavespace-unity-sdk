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

    /// <summary>
    /// KEMAR head radius (metres) passed to the native NFC filter.
    /// Must match AT::NearFieldCorrection::DEFAULT_HEAD_RADIUS in the C++ library.
    /// Passing 0 is NOT a "use default" convention on the native side: it gets
    /// clamped to 0.01 m (a 1 cm head), which collapses the inter-ear distance
    /// difference and silently neutralises the ILD correction.
    /// </summary>
    public const float KEMAR_HEAD_RADIUS = 0.0875f;
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
    [NonSerialized] public bool isPrevSimpleBinauralSpat;
    public bool   isNearFieldCorrection;
    // NonSerialized for the same reason as isPrevSimpleBinauralSpat: "prev"
    // change-detection state must never be restored by Unity serialization,
    // otherwise a restored prev == current means the value is never sent to a
    // freshly created native engine (NFC silently off despite the checkbox,
    // or rRef stuck at the native default of 1.0 m).
    [NonSerialized] public bool  isPrevIsNearFieldCorrection;
    public float  hrtfDistance;
    [NonSerialized] public float prevHrtfDistance = -1f;  // -1 forces send on first Update
    public string hrtfFilePath = "";
    public bool   isHrtfTruncated;

    /// <summary>
    /// Stereo-downmix rendering algorithm: 0 = amplitude panning (default,
    /// no HRTF/convolution/delay lines), 1 = HRTF convolution. Falls back to
    /// amplitude panning automatically (native side) if 1 is selected but no
    /// HRTF file has been loaded yet.
    /// </summary>
    public int binauralRenderMode = 0;
    public int    numVirtualSpeakers = 2;

    public bool    isPlaying     = false;
    public bool    isInitialized = false;

    /// <summary>
    /// Which internal bus AT_WS_startRecording() taps — see the native
    /// AT::SpatializationEngine::RecordingSource doc comment for the exact
    /// tap points. Values match the native enum's underlying ints exactly.
    /// </summary>
    public enum RecordingSource
    {
        Downmix          = 0, // final 2-channel bus — requires binaural virtualization enabled
        RawMultichannel  = 1, // raw WFS multichannel bus, pre-downmix, pre-master-gain
        SimulatedCapture = 2, // same bus as RawMultichannel, free-field capture-simulation model
    }

    /// <summary>File format for AT_WS_startRecording(). Matches the native enum.</summary>
    public enum RecordingFormat
    {
        Wav    = 0,
        Vorbis = 1,
    }

    /// <summary>True while a recording is in progress. Polled from AT_WS_isRecording() — see Update().</summary>
    public bool isRecording = false;

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

        // NOTE: isPrevSimpleBinauralSpat is intentionally NOT force-inverted here.
        // InitSpatializerEngine() (called a few lines below) already sends the
        // initial isSimpleBinauralSpat value to the native engine once. If we also
        // forced a mismatch here, the first Update() would detect a "change" and
        // call AT_WS_setIsSimpleBinauralSpat() a second time with the exact same
        // value — a fully redundant call. On the native side this used to
        // unconditionally re-arm the fade-out/reset/warmup/fade-in transition
        // machinery for nothing (clicks + a few seconds of silence at PlayMode
        // start, even with a perfectly static listener). The native engine now
        // also guards against this (no-ops a request for the already-targeted
        // mode), but there is no reason to make the redundant P/Invoke call at all.
        // isPrevSimpleBinauralSpat is set to match isSimpleBinauralSpat right after
        // InitSpatializerEngine() actually sends it (see below), so Update()'s
        // change-detection starts in sync with the engine's real state.

        meters = new float[outputChannelCount];

        virtualSpeakers = FindObjectsOfType<At_VirtualSpeaker>();
        foreach (At_VirtualSpeaker vs in virtualSpeakers)
            vs.m_maxDistanceForDelay = outputState.maxDistanceForDelay;

        InitSpatializerEngine();

        // Abort the rest of Awake() when the engine failed to start (e.g. channel
        // count mismatch, device not available).  isInitialized is false at this
        // point; proceeding would call addPlayer() which returns -1 without assigning
        // p.masterOutput, and the subsequent initPlayer() call would throw a
        // NullReferenceException on masterOutput.numVirtualSpeakers.
        if (!isInitialized) return;

        // Push virtual speaker positions to the native engine BEFORE any
        // player initializes — critical for 6DOF source masking: initPlayer()
        // (below) can enable masking and trigger a ONE-TIME snapshot of
        // m_virtualSpeakerPositionsFlat (see AT_SpatPlayer::prepare6dofMask).
        // If that snapshot happens before real positions are ever pushed,
        // every mic is captured at (0,0,0) — a permanently degenerate
        // geometry that never self-corrects (guarded by
        // m_is6dofMaskPrepared). WFS/binaural rendering was unaffected by
        // the old ordering because it re-reads speaker positions live every
        // frame elsewhere in the engine; 6DOF masking does not.
        UpdateVirtualSpeakerPosition();

        // Query scene players AFTER the engine has started successfully.
        // Placing this call here rather than at the top of Awake() has two benefits:
        //   1. Players whose own Awake() has not yet run still have their masterOutput
        //      reference unset; initPlayer() handles this via a defensive null-check.
        //   2. We avoid iterating (and silently skipping) players when the engine is
        //      not running, which would leave them permanently uninitialized.
        At_Player[] players = FindObjectsOfType<At_Player>();

        foreach (At_Player p in players)
        {
            if (!p.isInitialized)
            {
                // addPlayer() returns -1 when registration fails (e.g. duplicate GUID).
                // Guard initPlayer() on success to prevent it from running with a
                // partially-configured player (masterOutput not yet assigned).
                if (addPlayer(p) != -1)
                    p.initPlayer();
            }
        }

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

        AT_WS_setMasterGain(gain);
        AT_WS_setMakeupMasterGain(makeupGain);

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
            if (AT_WS_setNearFieldCorrectionRRef(hrtfDistance, KEMAR_HEAD_RADIUS) == AUDIO_PLUGIN_ERROR)
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
            AT_WS_setSecondarySourceSize(secondarySourceSize);

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

        // Check the setup result before doing anything else.  A failed setup means
        // the JUCE device manager is not running; calling LoadHRTF or
        // setIsSimpleBinauralSpat on it would be unsafe and is likely to crash.
        if (result != AUDIO_PLUGIN_OK)
        {
            Debug.LogError($"[AudioPlugin] Failed to setup audio device '{audioDeviceName}' " +
                           $"with {numVirtualSpeakers} virtual speaker(s) " +
                           $"(isBinaural={isBinauralVirtualization}). " +
                           "Verify that the device has enough output channels, " +
                           "or enable Binaural Virtualization.");
            AT_WS_shutdown();
            return;
        }

        if (isBinauralVirtualization)
        {
            SetBinauralRenderMode(binauralRenderMode);

            // Only load a file if HRTF mode is actually selected AND a
            // previously-chosen file is known. Otherwise: amplitude panning
            // (mode 0), or HRTF mode with no file loaded yet — the native
            // side already falls back to amplitude panning automatically in
            // that case (see SpatializationEngine::processBinauralVirtualization),
            // so there is nothing to load here. "Use Default HRTF" no longer
            // exists as a concept — see the render-mode dropdown instead.
            if (binauralRenderMode == 1 && !string.IsNullOrEmpty(hrtfFilePath) && System.IO.File.Exists(hrtfFilePath))
                LoadHRTFFile(hrtfFilePath);

            if (AT_WS_setIsSimpleBinauralSpat(isSimpleBinauralSpat) == AUDIO_PLUGIN_ERROR)
                Debug.LogError($"[AudioPlugin] Failed to set simple binaural mode to {isSimpleBinauralSpat}");

            // ── Near-field correction: send initial state explicitly here, same
            // pattern (and same rationale) as isSimpleBinauralSpat above — a
            // freshly created native engine starts with NFC off and rRef=1.0,
            // regardless of what the previous session sent. Update()'s
            // change-detection then starts in sync via the prev-value syncs below.
            if (hrtfDistance <= 0f)
            {
                Debug.LogWarning("[AudioPlugin] hrtfDistance <= 0 in saved output state — " +
                                 "falling back to 1.0 m (BRIR reference distance)");
                hrtfDistance = 1.0f;
            }
            if (AT_WS_setNearFieldCorrectionRRef(hrtfDistance, KEMAR_HEAD_RADIUS) == AUDIO_PLUGIN_ERROR)
                Debug.LogError($"[AudioPlugin] Failed to set NFC rRef to {hrtfDistance}");
            if (AT_WS_setIsNearFieldCorrection(isNearFieldCorrection) == AUDIO_PLUGIN_ERROR)
                Debug.LogError($"[AudioPlugin] Failed to set near-field correction to {isNearFieldCorrection}");

            prevHrtfDistance            = hrtfDistance;
            isPrevIsNearFieldCorrection = isNearFieldCorrection;
        }

        // Engine now reflects isSimpleBinauralSpat (sent above when binaural
        // virtualization is on, or simply never required otherwise since WFS-only
        // is already the engine's default). Sync the prev-value so Update()'s
        // change-detection doesn't redundantly resend it on the first frame.
        isPrevSimpleBinauralSpat = isSimpleBinauralSpat;

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

        AT_WS_setVirtualSpeakerTransform(m_speakerPositions, m_speakerRotations, m_speakerForwards, numVirtualSpeakers);
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
        }
        else
        {
            Debug.LogError("[AT_WS] Failed to load default HRTF");
        }
    }

    /// <summary>
    /// Selects the stereo-downmix rendering algorithm: 0 = amplitude panning
    /// (default), 1 = HRTF. Falls back to amplitude panning automatically
    /// (native side) if 1 is selected but no HRTF file has been loaded yet.
    /// </summary>
    public void SetBinauralRenderMode(int mode)
    {
        if (AT_WS_setBinauralRenderMode(mode) != AUDIO_PLUGIN_OK)
            Debug.LogError($"[AT_WS] Failed to set binaural render mode to {mode}");
    }
    #endregion

    #region Recording
    // See AT::SpatializationEngine::startRecording() (native) for the full
    // contract — exact tap points per RecordingSource, format handling.
    // Real-time only: recording must be started/stopped while the engine is
    // already running (Play Mode) — there is no offline rendering path.

    /// <summary>
    /// Starts real-time recording of the engine's own output to a file.
    /// NOT real-time safe (allocates a file writer) — call from the main/Unity
    /// thread only, e.g. an Editor window button, never from an audio callback.
    /// </summary>
    /// <param name="filePath">Destination file path (extension not enforced — pick one matching format).</param>
    /// <param name="source">Which internal bus to tap.</param>
    /// <param name="format">WAV (PCM) or Ogg Vorbis.</param>
    /// <param name="bitDepthOrQuality">WAV: bit depth (16/24/32). Vorbis: quality index (0-10).</param>
    /// <returns>true if recording started successfully.</returns>
    public bool StartRecording(string filePath, RecordingSource source, RecordingFormat format, int bitDepthOrQuality)
    {
        if (string.IsNullOrEmpty(filePath))
        {
            Debug.LogError("[AT_WS] StartRecording: file path is empty");
            return false;
        }

        bool ok = AT_WS_startRecording(filePath, (int)source, (int)format, bitDepthOrQuality) == AUDIO_PLUGIN_OK;
        if (ok) isRecording = true;
        else    Debug.LogError($"[AT_WS] Failed to start recording to {filePath} "
                               + $"(source={source}, format={format}) — see native log for the exact reason "
                               + "(e.g. Downmix requires binaural virtualization to be enabled).");
        return ok;
    }

    /// <summary>Stops the current recording, if any. Safe to call even if not recording.</summary>
    public void StopRecording()
    {
        if (AT_WS_stopRecording() != AUDIO_PLUGIN_OK)
            Debug.LogError("[AT_WS] Failed to stop recording");
        isRecording = false;
    }

    /// <summary>Refreshes the `isRecording` field from the native engine. Cheap — safe to call every frame/OnGUI.</summary>
    public void RefreshIsRecording()
    {
        isRecording = AT_WS_isRecording() != 0;
    }

    /// <summary>Number of samples (per channel) written so far in the current/last recording.</summary>
    public long GetRecordingSamplesWritten() => AT_WS_getRecordingSamplesWritten();
    #endregion

    #region Metering
    /// <summary>Reads output RMS meter values from the native library into the provided array.</summary>
    public unsafe void getMeters(float[] meters, int arraySize)
    {
        fixed (float* ptr = meters)
        {
            AT_WS_getMixerOutputMeters((IntPtr)ptr, arraySize);
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
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int AT_WS_startRecording(string filePath, int source, int format, int bitDepthOrQuality);
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int AT_WS_stopRecording();
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int AT_WS_isRecording();
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern long AT_WS_getRecordingSamplesWritten();
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int AT_WS_setMasterGain(float masterGain);
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int AT_WS_setMakeupMasterGain(float makeupMasterGain);
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int AT_WS_setVirtualSpeakerTransform(float[] positions, float[] rotations, float[] forwards, int count);
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int AT_WS_getMixerOutputMeters(IntPtr meters, int arraySize);
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int AT_WS_setIsBinauralVirtualization(bool isBinauralVirtualization);
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall, CharSet = CharSet.Ansi)] private static extern int AT_WS_loadHRTF(string filePath);
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int AT_WS_loadDefaultHRTF();
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int AT_WS_setBinauralRenderMode(int mode);
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
