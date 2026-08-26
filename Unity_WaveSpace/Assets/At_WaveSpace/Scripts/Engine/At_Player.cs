/// @file At_Player.cs
/// @brief Multichannel audio player and WFS/binaural spatializer.
///
/// @details
/// Each instance corresponds to one audio source in the scene. It communicates
/// with the AT SPAT native library to stream audio, apply WFS spatialization,
/// and expose per-channel metering.
///
/// WFS parameter arrays (delayArray, volumeArray, activationSpeakerVolume) are
/// pre-allocated at the maximum size (MAX_VIRTUAL_SPEAKERS) to avoid runtime GC.
/// They must stay synchronized with At_MasterOutput.MAX_VIRTUAL_SPEAKERS and
/// MAX_VIRTUAL_SPEAKERS in the C++ library.

using System.Collections.Generic;
using UnityEngine;
using UnityEditor;
using System;
using System.Runtime.InteropServices;
using UnityEngine.SceneManagement;

public class At_Player : MonoBehaviour
{
    #region Constants
    private const int AUDIO_PLUGIN_OK    = 0;
    private const int AUDIO_PLUGIN_ERROR = 1;

    /// <summary>
    /// Maximum number of WFS channels. Must match At_MasterOutput.MAX_VIRTUAL_SPEAKERS
    /// and MAX_VIRTUAL_SPEAKERS in the C++ library.
    /// </summary>
    private const int MAX_VIRTUAL_SPEAKERS = 1024;
    #endregion

    #region Public Variables — Audio Configuration
    /// <summary>Path to the audio file (must be in StreamingAssets).</summary>
    public string fileName;

    /// <summary>Source gain (dB).</summary>
    public float gain;

    /// <summary>True = 3D WFS spatialization; false = 2D direct output.</summary>
    public bool is3D;

    /// <summary>If true, playback starts automatically on Awake.</summary>
    public bool isPlayingOnAwake;

    /// <summary>If true, the audio file loops.</summary>
    public bool isLooping;

    /// <summary>Distance attenuation exponent.</summary>
    public float attenuation;

    /// <summary>Minimum distance below which no attenuation is applied (meters).</summary>
    public float minDistance;

    /// <summary>Playback speed multiplier (1.0 = normal speed).</summary>
    public float playbackSpeed;

    /// <summary>If true, the source continuously rotates to face the listener.</summary>
    public bool isLookAtListener;

    
    

    /// <summary>Low-pass filter cutoff frequency (Hz).</summary>
    public float lowPassFc;

    /// <summary>Low-pass filter gain (dB).</summary>
    public float lowPassGain;

    /// <summary>High-pass filter cutoff frequency (Hz).</summary>
    public float highPassFc;

    /// <summary>High-pass filter gain (dB).</summary>
    public float highPassGain;

    /// <summary>If true, the low-pass filter is bypassed.</summary>
    public bool lowPassBypass;

    /// <summary>If true, the high-pass filter is bypassed.</summary>
    public bool highPassBypass;

    /// <summary>
    /// If true, applies listener-position-dependent 6DOF source masking to
    /// this player's output (2D mode only). See AT_SixDofMaskProcessor.
    /// </summary>
    public bool is6dofMaskEnabled;

    /// <summary>Grid resolution (metres) for 6DOF source mode-detection (temporal history, not the MUSIC spatial search grid).</summary>
    public float sixDofGridRes;

    /// <summary>Minimum matching-estimate count for a 6DOF source to be accepted.</summary>
    public int sixDofMinBlockCount;

    /// <summary>Number of audio blocks buffered into one 6DOF localization window.</summary>
    public int sixDofNumBufferedBlocks;

    /// <summary>Number of sources the MUSIC signal subspace is sized for / peak search cap.</summary>
    public int sixDofMaxSources;

    /// <summary>Spatial resolution (metres) of the MUSIC candidate-position search grid.</summary>
    public float sixDofSearchGridResolution;

    /// <summary>Number of frequency bands combined per MUSIC analysis window.</summary>
    public int sixDofMaxBins;

    /// <summary>Lower bound (Hz) of the frequency range analyzed by MUSIC.</summary>
    public float sixDofBandHzMin;

    /// <summary>Upper bound (Hz) of the frequency range analyzed by MUSIC.</summary>
    public float sixDofBandHzMax;

    /// <summary>Lower bound (metres) of the height range swept by the MUSIC search grid.</summary>
    public float sixDofYRangeMin;

    /// <summary>Upper bound (metres) of the height range swept by the MUSIC search grid.</summary>
    public float sixDofYRangeMax;
    #endregion

    #region Public Variables — 6DOF Detected Sources (Runtime State)
    /// <summary>
    /// Maximum number of simultaneously detected 6DOF sources. Must match
    /// AT::SixDofMaskProcessor::MAX_SOURCES in the C++ library.
    /// </summary>
    public const int MAX_6DOF_SOURCES = 6;

    /// <summary>
    /// Number of 6DOF sources currently detected by the native processor
    /// (0 if masking is disabled, not yet detected, or in bypass fallback
    /// after repeated detection failures — see isInBypassFallback semantics
    /// in AT_SixDofMaskProcessor). Updated once per Update() via
    /// Refresh6dofSourcePositions().
    /// </summary>
    public int num6dofDetectedSources = 0;

    /// <summary>
    /// Flat [x0,y0,z0,x1,y1,z1,...] positions of currently detected 6DOF
    /// sources, world/engine frame. Only the first `num6dofDetectedSources`
    /// entries are valid. Pre-allocated to MAX_6DOF_SOURCES*3 to avoid
    /// per-frame GC allocation, same rationale as delayArray/volumeArray.
    /// </summary>
    public readonly float[] sixDofSourcePositionsFlat = new float[MAX_6DOF_SOURCES * 3];
    #endregion

    #region Public Variables — Runtime State
    /// <summary>Spatial ID assigned by the native engine on registration.</summary>
    public int spatID;

    /// <summary>True while the player is streaming audio.</summary>
    public bool isPlaying = false;

    /// <summary>Number of output channels in the current audio device configuration.</summary>
    public int outputChannelCount;

    /// <summary>True when this instance was created dynamically at runtime.</summary>
    public bool isDynamicInstance = false;

    /// <summary>Number of channels in the loaded audio file.</summary>
    public int numChannelsInAudioFile = 0;

    /// <summary>RMS meter values per audio channel (dB).</summary>
    public float[] meters;

    /// <summary>Reference to the scene master output.</summary>
    public At_MasterOutput masterOutput;

    /// <summary>Persistent unique identifier for this player instance.</summary>
    public string guid = "";

    /// <summary>True once the player has been registered and initialized with the engine.</summary>
    public bool isInitialized = false;

    /// <summary>Number of WFS virtual speakers currently in use.</summary>
    public int numVirtualSpeakers = 2;

    // Pre-allocated WFS arrays — indexed by speaker ID, sized to MAX_VIRTUAL_SPEAKERS.
    /// <summary>Speaker activation mask (1 = active, 0 = masked) for each WFS channel.</summary>
    public readonly float[] activationSpeakerVolume = new float[MAX_VIRTUAL_SPEAKERS];

    /// <summary>WFS delay values (seconds) for each output channel.</summary>
    public readonly float[] delayArray = new float[MAX_VIRTUAL_SPEAKERS];

    /// <summary>WFS linear gain values for each output channel.</summary>
    public readonly float[] volumeArray = new float[MAX_VIRTUAL_SPEAKERS];

    // Unused global counter kept for potential external tooling.
    static public int playerCount;
    #endregion

    #region Private Variables
    private At_PlayerState playerState;
    private string objectName;
    private At_Listener listener;
    #endregion

    #region Unity Lifecycle
    /// <summary>Generates a new GUID when the component is reset or first added.</summary>
    private void Reset() => setGuid();

    /// <summary>
    /// Re-generates the GUID on Duplicate or prefab drag to guarantee uniqueness.
    /// </summary>
    private void OnValidate()
    {
        Event e = Event.current;
        if (e == null) return;

        if ((e.type == EventType.ExecuteCommand && e.commandName == "Duplicate") ||
             e.type == EventType.DragPerform)
        {
            setGuid();
        }
    }

    public void Awake()
    {
        // Set up scene references only.  Do NOT call addPlayer() or initPlayer() here.
        //
        // Initialization responsibility is split by instantiation type:
        //   - Scene players  : At_MasterOutput.Awake() iterates FindObjectsOfType and
        //                       calls addPlayer() + initPlayer() for each player after
        //                       the engine has started successfully.
        //   - Runtime players: OnEnable() fires after this Awake(), by which time
        //                       masterOutput is already assigned; it calls addPlayer() +
        //                       initPlayer() when the engine is running.
        //
        // Self-initializing here would create a dual-init race with
        // At_MasterOutput.Awake() because Unity does not guarantee the Awake()
        // execution order across GameObjects.
        listener     = FindObjectOfType<At_Listener>();
        objectName   = gameObject.name;
        masterOutput = FindObjectOfType<At_MasterOutput>();
    }

    public void OnEnable()
    {
        // Handles players instantiated at runtime after the engine is already running.
        // For scene players this path is inert: At_MasterOutput.Awake() sets
        // isInitialized = true before OnEnable() fires, so the guard below exits early.
        // masterOutput is assigned by Awake() which always runs before OnEnable().
        if (masterOutput == null || !masterOutput.isInitialized || isInitialized) return;

        if (masterOutput.addPlayer(this) != -1)
            initPlayer();
    }

    /// <summary>Stops playback and unregisters the player from the native engine.</summary>
    public void OnDisable()
    {
        isPlaying = false;

        if (masterOutput != null && masterOutput.isInitialized)
        {
            AT_WS_stopPlayer(spatID);

            if (AT_WS_removePlayer(spatID) != AUDIO_PLUGIN_OK)
                Debug.LogError($"[AudioPlugin] Failed to remove player {spatID}");

            masterOutput.removePlayerFromListWithSpatID(spatID);
            isInitialized = false;
        }
    }

    private void Update()
    {
        if (!isInitialized) return;

        getMeters(spatID, meters, numChannelsInAudioFile);

        AT_WS_setPlayerRealTimeParameter(spatID, gain, playbackSpeed, attenuation, minDistance);
        Update6dofMaskParameters();

        if (is6dofMaskEnabled)
            Refresh6dofSourcePositions();

        if (is3D)
        {
            UpdateSpatialParameters();

            if (isLookAtListener && listener != null)
                transform.LookAt(listener.gameObject.transform);
        }
    }
    #endregion

    #region Playback Control
    /// <summary>Starts audio playback.</summary>
    public void StartPlaying()
    {
        if (masterOutput == null || !masterOutput.isInitialized) return;

        if (AT_WS_startPlayer(spatID) == AUDIO_PLUGIN_OK)
            isPlaying = true;
        else
            Debug.LogError($"[AudioPlugin] Failed to start player {spatID}");
    }

    /// <summary>Stops audio playback.</summary>
    public void StopPlaying()
    {
        if (masterOutput == null || !masterOutput.isInitialized) return;

        if (AT_WS_stopPlayer(spatID) == AUDIO_PLUGIN_OK)
            isPlaying = false;
        else
            Debug.LogError($"[AudioPlugin] Failed to stop player {spatID}");
    }
    #endregion

    #region Initialization
    /// <summary>Generates a new unique GUID for this player.</summary>
    public void setGuid() => guid = System.Guid.NewGuid().ToString();

    /// <summary>Allocates the meters array to match the current audio file channel count.</summary>
    public void initMeters() => meters = new float[numChannelsInAudioFile];

    /// <summary>
    /// Loads persistent state, configures the native player, and optionally starts playback.
    /// Called by At_MasterOutput.Awake() after the engine is initialized.
    /// </summary>
    public void initPlayer()
    {
        // masterOutput is assigned by Awake() before initPlayer() is ever called
        // (either via At_MasterOutput.Awake() for scene players, or via OnEnable()
        // for runtime players).  The null check below is a defensive fallback for
        // exceptional cases (e.g. initPlayer() called directly from editor tooling).
        if (masterOutput == null)
            masterOutput = FindObjectOfType<At_MasterOutput>();

        if (masterOutput == null)
        {
            Debug.LogError($"[AudioPlugin] initPlayer(): At_MasterOutput not found in scene " +
                           $"for player '{gameObject.name}'. Initialization skipped.");
            return;
        }

        numVirtualSpeakers = masterOutput.numVirtualSpeakers;

        playerState = At_AudioEngineUtils.getPlayerStateWithGuidAndName(
            SceneManager.GetActiveScene().name, guid, gameObject.name);

        if (playerState != null && !string.IsNullOrEmpty(playerState.fileName))
        {
            // playerState.fileName is stored relative to StreamingAssets (e.g. "Audio/MyFile.wav").
            // Reconstruct the absolute path so native calls (AT_WS_setPlayerFilePath,
            // AT_WS_getAudioFileMetadata) receive a valid filesystem path on any machine / OS.
            fileName = System.IO.Path.Combine(Application.streamingAssetsPath, playerState.fileName);
            gain                   = playerState.gain;
            is3D                   = playerState.is3D;
            isPlayingOnAwake       = playerState.isPlayingOnAwake;
            isLooping              = playerState.isLooping;
            attenuation            = playerState.attenuation;
            minDistance            = playerState.minDistance;
            lowPassFc              = playerState.lowPassFc;
            highPassFc             = playerState.highPassFc;
            lowPassGain            = playerState.lowPassGain;
            numChannelsInAudioFile = playerState.numChannelsInAudiofile;
            is6dofMaskEnabled      = playerState.is6dofMaskEnabled;
            sixDofGridRes          = playerState.sixDofGridRes;
            sixDofMinBlockCount    = playerState.sixDofMinBlockCount;
            sixDofNumBufferedBlocks = playerState.sixDofNumBufferedBlocks;
            sixDofMaxSources       = playerState.sixDofMaxSources;
            sixDofSearchGridResolution = playerState.sixDofSearchGridResolution;
            sixDofMaxBins          = playerState.sixDofMaxBins;
            sixDofBandHzMin        = playerState.sixDofBandHzMin;
            sixDofBandHzMax        = playerState.sixDofBandHzMax;
            sixDofYRangeMin        = playerState.sixDofYRangeMin;
            sixDofYRangeMax        = playerState.sixDofYRangeMax;
        }

        // Fallback: read the audio file metadata if serialized data are not valid.
        if (numChannelsInAudioFile <= 0 && !string.IsNullOrEmpty(fileName))
        {
            int ch;
            double sr, len;
            long samples;
            if (AT_WS_getAudioFileMetadata(fileName, out ch, out sr, out len, out samples) == AUDIO_PLUGIN_OK)
                numChannelsInAudioFile = ch;
        }

        outputChannelCount = masterOutput.outputChannelCount;

        // delayArray, volumeArray, activationSpeakerVolume are pre-allocated at field init.
        meters = new float[numChannelsInAudioFile];

        AT_WS_setPlayerRealTimeParameter(spatID, gain, playbackSpeed, attenuation, minDistance);

        if (AT_WS_setPlayerFilePath(spatID, fileName) != AUDIO_PLUGIN_OK)
            Debug.LogError($"[AudioPlugin] Failed to set file path for player {spatID}: {fileName}");

        initMeters();

        UpdateSpatialParameters();

        // Must happen AFTER AT_WS_setPlayerFilePath(): 6DOF masking needs the
        // player's channel count, which the native side only knows once the
        // audio file is loaded (see SpatPlayer::prepare6dofMask).
        Update6dofMaskParameters();

        if (isPlayingOnAwake) StartPlaying();

        isInitialized = true;
    }
    #endregion

    #region WFS Parameter Queries
    /// <summary>Retrieves WFS delay values for all output channels from the native library.</summary>
    public unsafe void getDelay(int id, float[] delay, int arraySize)
    {
        fixed (float* ptr = delay)
        {
            AT_WS_getPlayerWfsDelay(id, (IntPtr)ptr, arraySize);
        }
    }

    /// <summary>Retrieves WFS linear gain values for all output channels from the native library.</summary>
    public unsafe void getVolume(int id, float[] volume, int arraySize)
    {
        fixed (float* ptr = volume)
        {
            AT_WS_getPlayerWfsLinGain(id, (IntPtr)ptr, arraySize);
        }
    }

    /// <summary>Retrieves the speaker activation mask for this source from the native library.</summary>
    public unsafe void getSpeakerMask(int id, float[] speakerMask, int arraySize)
    {
        fixed (float* ptr = speakerMask)
        {
            AT_WS_getPlayerSpeakerMask(id, (IntPtr)ptr, arraySize);
        }
    }

    /// <summary>Retrieves RMS meter values for all audio file channels from the native library.</summary>
    public unsafe void getMeters(int id, float[] meters, int arraySize)
    {
        fixed (float* ptr = meters)
        {
            if (arraySize <= 0 || meters == null || meters.Length == 0) return;
            AT_WS_getPlayerMeters(id, (IntPtr)ptr, arraySize);
        }
    }
    #endregion

    #region 6DOF Source Masking
    // Cache of the last values actually pushed to the native side — avoids
    // re-triggering AT_WS_setPlayer6dofMaskEnabled(uid, true) every frame,
    // which would otherwise re-run the native prepare() path repeatedly
    // (full buffer realloc + background-thread restart, unsafe concurrently
    // with the audio thread) — see AT_SpatPlayer::prepare6dofMask guard.
    private bool  m_last6dofEnabled;
    private float m_last6dofGridRes;
    private int   m_last6dofMinBlockCount;
    private int   m_last6dofNumBufferedBlocks;
    private int   m_last6dofMaxSources;
    private float m_last6dofSearchGridResolution;
    private int   m_last6dofMaxBins;
    private float m_last6dofBandHzMin;
    private float m_last6dofBandHzMax;
    private float m_last6dofYRangeMin;
    private float m_last6dofYRangeMax;
    private bool  m_6dofParamsPushedOnce;

    /// <summary>
    /// Pushes the current 6DOF masking parameters (enabled flag + all MUSIC
    /// localization parameters) to the native player, but ONLY when they
    /// actually changed since the last call — safe to call every Update()
    /// without re-triggering native (re-)allocation each frame.
    /// </summary>
    public void Update6dofMaskParameters()
    {
        bool unchanged = m_6dofParamsPushedOnce
            && m_last6dofEnabled == is6dofMaskEnabled
            && Mathf.Approximately(m_last6dofGridRes, sixDofGridRes)
            && m_last6dofMinBlockCount == sixDofMinBlockCount
            && m_last6dofNumBufferedBlocks == sixDofNumBufferedBlocks
            && m_last6dofMaxSources == sixDofMaxSources
            && Mathf.Approximately(m_last6dofSearchGridResolution, sixDofSearchGridResolution)
            && m_last6dofMaxBins == sixDofMaxBins
            && Mathf.Approximately(m_last6dofBandHzMin, sixDofBandHzMin)
            && Mathf.Approximately(m_last6dofBandHzMax, sixDofBandHzMax)
            && Mathf.Approximately(m_last6dofYRangeMin, sixDofYRangeMin)
            && Mathf.Approximately(m_last6dofYRangeMax, sixDofYRangeMax);
        if (unchanged) return;

        AT_WS_setPlayer6dofMaskEnabled(spatID, is6dofMaskEnabled);
        if (is6dofMaskEnabled)
        {
            AT_WS_setPlayer6dofGridRes(spatID, sixDofGridRes);
            AT_WS_setPlayer6dofMinBlockCount(spatID, sixDofMinBlockCount);
            AT_WS_setPlayer6dofNumBufferedBlocks(spatID, sixDofNumBufferedBlocks);
            AT_WS_setPlayer6dofMaxSources(spatID, sixDofMaxSources);
            AT_WS_setPlayer6dofSearchGridResolution(spatID, sixDofSearchGridResolution);
            AT_WS_setPlayer6dofMaxBins(spatID, sixDofMaxBins);
            AT_WS_setPlayer6dofBandHzMin(spatID, sixDofBandHzMin);
            AT_WS_setPlayer6dofBandHzMax(spatID, sixDofBandHzMax);
            AT_WS_setPlayer6dofYRangeMin(spatID, sixDofYRangeMin);
            AT_WS_setPlayer6dofYRangeMax(spatID, sixDofYRangeMax);
        }

        m_last6dofEnabled              = is6dofMaskEnabled;
        m_last6dofGridRes              = sixDofGridRes;
        m_last6dofMinBlockCount        = sixDofMinBlockCount;
        m_last6dofNumBufferedBlocks    = sixDofNumBufferedBlocks;
        m_last6dofMaxSources           = sixDofMaxSources;
        m_last6dofSearchGridResolution = sixDofSearchGridResolution;
        m_last6dofMaxBins              = sixDofMaxBins;
        m_last6dofBandHzMin            = sixDofBandHzMin;
        m_last6dofBandHzMax            = sixDofBandHzMax;
        m_last6dofYRangeMin            = sixDofYRangeMin;
        m_last6dofYRangeMax            = sixDofYRangeMax;
        m_6dofParamsPushedOnce         = true;
    }

    /// <summary>
    /// Refreshes num6dofDetectedSources and sixDofSourcePositionsFlat from
    /// the native processor. Cheap (a handful of floats) — safe to call
    /// every Update() while masking is enabled, mirroring getMeters().
    /// Logs to the console when the detected set actually changes (not every
    /// frame) — see m_last6dofLoggedCount/m_last6dofLoggedPositions.
    /// </summary>
    public unsafe void Refresh6dofSourcePositions()
    {
        if (!is6dofMaskEnabled) { num6dofDetectedSources = 0; return; }

        AT_WS_getPlayer6dofSourceCount(spatID, out num6dofDetectedSources);

        fixed (float* ptr = sixDofSourcePositionsFlat)
        {
            AT_WS_getPlayer6dofSourcePositions(spatID, (IntPtr)ptr, sixDofSourcePositionsFlat.Length);
        }

        LogDetectedSourcesIfChanged();
    }

    // Cache used only to decide whether the detected set changed enough to
    // warrant a new console log line — avoids spamming Debug.Log every frame
    // while sources are stable (Update() calls Refresh6dofSourcePositions()
    // every frame).
    private int m_last6dofLoggedCount = -1;
    private readonly float[] m_last6dofLoggedPositions = new float[MAX_6DOF_SOURCES * 3];
    private const float LOG_POSITION_EPSILON = 0.01f; // metres

    private void LogDetectedSourcesIfChanged()
    {
        bool changed = num6dofDetectedSources != m_last6dofLoggedCount;
        if (!changed)
        {
            for (int i = 0; i < num6dofDetectedSources * 3; i++)
            {
                if (Mathf.Abs(sixDofSourcePositionsFlat[i] - m_last6dofLoggedPositions[i]) > LOG_POSITION_EPSILON)
                {
                    changed = true;
                    break;
                }
            }
        }
        if (!changed) return;

        System.Array.Copy(sixDofSourcePositionsFlat, m_last6dofLoggedPositions, sixDofSourcePositionsFlat.Length);
        m_last6dofLoggedCount = num6dofDetectedSources;

        if (num6dofDetectedSources == 0)
        {
            Debug.Log($"[6DOF] Player '{gameObject.name}' (uid {spatID}): no source detected.");
            return;
        }

        var sb = new System.Text.StringBuilder();
        sb.Append($"[6DOF] Player '{gameObject.name}' (uid {spatID}): {num6dofDetectedSources} source(s) detected — ");
        for (int i = 0; i < num6dofDetectedSources; i++)
        {
            sb.Append($"S{i}=({sixDofSourcePositionsFlat[i * 3 + 0]:F2}, " +
                              $"{sixDofSourcePositionsFlat[i * 3 + 1]:F2}, " +
                              $"{sixDofSourcePositionsFlat[i * 3 + 2]:F2})");
            if (i < num6dofDetectedSources - 1) sb.Append(", ");
        }
        Debug.Log(sb.ToString());
    }

    /// <summary>
    /// Public getter for another class (e.g. a Gizmo/visualization component)
    /// to read the currently detected 6DOF source positions without
    /// duplicating the native call. Allocates a small managed array —
    /// intended for occasional Editor/Gizmo use, NOT per-frame audio-path code
    /// (use sixDofSourcePositionsFlat + num6dofDetectedSources directly there).
    /// </summary>
    /// <returns>World-space positions of currently detected 6DOF sources (may be empty).</returns>
    public Vector3[] Get6dofSourcePositions()
    {
        var result = new Vector3[num6dofDetectedSources];
        for (int i = 0; i < num6dofDetectedSources; i++)
        {
            result[i] = new Vector3(
                sixDofSourcePositionsFlat[i * 3 + 0],
                sixDofSourcePositionsFlat[i * 3 + 1],
                sixDofSourcePositionsFlat[i * 3 + 2]);
        }
        return result;
    }

    /// <summary>Number of currently detected 6DOF sources (0 if masking is disabled).</summary>
    public int Get6dofSourceCount() => num6dofDetectedSources;
    #endregion

    #region Spatialization
    /// <summary>
    /// Queries WFS parameters from the engine, updates speaker active/inactive state,
    /// and sends the current source transform to the native library.
    /// </summary>
    public void UpdateSpatialParameters()
    {
        getDelay(spatID, delayArray, numVirtualSpeakers);
        getVolume(spatID, volumeArray, numVirtualSpeakers);
        getSpeakerMask(spatID, activationSpeakerVolume, numVirtualSpeakers);

        for (int ch = 0; ch < numVirtualSpeakers; ch++)
        {
            At_VirtualSpeaker vs = masterOutput.speakerWithIndex(ch);
            if (vs != null)
                vs.isActive = activationSpeakerVolume[ch] != 0f;
        }

        float[] position = { transform.position.x, transform.position.y, transform.position.z };
        float[] rotation = { transform.rotation.x,  transform.rotation.y,  transform.rotation.z };
        float[] forward  = { transform.forward.x,   transform.forward.y,   transform.forward.z };

        AT_WS_setPlayerTransform(spatID, position, rotation, forward);
    }
    #endregion

    #region Gizmos
#if UNITY_EDITOR
    private void OnDrawGizmos()
    {
        if (is3D)
        {
            float distance;
            if (!isDynamicInstance)
            {
                At_PlayerState ps = At_AudioEngineUtils.getPlayerStateWithGuidAndName(
                    SceneManager.GetActiveScene().name, guid, gameObject.name);
                distance = ps != null ? ps.minDistance : 0f;
            }
            else
            {
                distance = minDistance;
            }

            const int STEPS = 20;
            float angle = 2f * Mathf.PI / STEPS;
            Gizmos.color = Color.green;
            for (int i = 0; i < STEPS; i++)
            {
                Vector3 p0 = transform.position + new Vector3(distance * Mathf.Cos(i * angle),       0f, distance * Mathf.Sin(i * angle));
                Vector3 p1 = transform.position + new Vector3(distance * Mathf.Cos((i + 1) * angle), 0f, distance * Mathf.Sin((i + 1) * angle));
                Gizmos.DrawLine(p0, p1);
            }
        }

        bool drewSomething = is3D;

        // 6DOF detected sources — 2D + masking enabled only. Positions come
        // from sixDofSourcePositionsFlat, refreshed every Update() via
        // Refresh6dofSourcePositions() (so this is only meaningful in Play
        // mode, while audio is actually streaming and being localized).
        if (!is3D && is6dofMaskEnabled && num6dofDetectedSources > 0)
        {
            const float SPHERE_DIAMETER = 0.1f;
            Gizmos.color = Color.magenta;
            for (int i = 0; i < num6dofDetectedSources; i++)
            {
                Vector3 pos = new Vector3(
                    sixDofSourcePositionsFlat[i * 3 + 0],
                    sixDofSourcePositionsFlat[i * 3 + 1],
                    sixDofSourcePositionsFlat[i * 3 + 2]);
                Gizmos.DrawSphere(pos, SPHERE_DIAMETER * 0.5f);
            }
            drewSomething = true;
        }

        if (drewSomething)
            SceneView.RepaintAll();
    }
#endif
    #endregion

    #region DLL Imports
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int AT_WS_startPlayer(int id);
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int AT_WS_stopPlayer(int id);
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int AT_WS_removePlayer(int uid);
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int AT_WS_setPlayerTransform(int id, float[] position, float[] rotation, float[] forward);
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int AT_WS_setPlayerRealTimeParameter(int uid, float gain, float playbackSpeed, float attenuation, float minDistance);
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int AT_WS_setPlayerFilePath(int uid, string path);
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int AT_WS_getPlayerNumChannel(int id, out int numChannel);
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int AT_WS_getPlayerWfsDelay(int id, IntPtr delay, int arraySize);
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int AT_WS_getPlayerWfsLinGain(int id, IntPtr linGain, int arraySize);
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int AT_WS_getPlayerSpeakerMask(int id, IntPtr speakerMask, int arraySize);
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int AT_WS_getPlayerMeters(int uid, IntPtr meter, int arraySize);
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int AT_WS_getAudioFileMetadata(string filepath, out int numChannels, out double sampleRate, out double lengthSeconds, out long totalSamples);
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int AT_WS_setPlayer6dofMaskEnabled(int uid, bool isEnabled);
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int AT_WS_setPlayer6dofGridRes(int uid, float gridRes);
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int AT_WS_setPlayer6dofMinBlockCount(int uid, int minBlockCount);
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int AT_WS_setPlayer6dofNumBufferedBlocks(int uid, int numBufferedBlocks);
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int AT_WS_setPlayer6dofMaxSources(int uid, int maxSources);
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int AT_WS_setPlayer6dofSearchGridResolution(int uid, float searchGridResolution);
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int AT_WS_setPlayer6dofMaxBins(int uid, int maxBins);
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int AT_WS_setPlayer6dofBandHzMin(int uid, float bandHzMin);
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int AT_WS_setPlayer6dofBandHzMax(int uid, float bandHzMax);
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int AT_WS_setPlayer6dofYRangeMin(int uid, float yRangeMin);
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int AT_WS_setPlayer6dofYRangeMax(int uid, float yRangeMax);
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int AT_WS_getPlayer6dofSourceCount(int uid, out int outCount);
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int AT_WS_getPlayer6dofSourcePositions(int uid, IntPtr positions, int arraySize);
    #endregion
}
