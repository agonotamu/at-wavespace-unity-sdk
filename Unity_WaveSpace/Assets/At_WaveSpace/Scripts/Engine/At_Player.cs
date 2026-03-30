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
            if (AT_WS_getPlayerWfsDelay(id, (IntPtr)ptr, arraySize) == AUDIO_PLUGIN_ERROR)
                Debug.LogError("[AudioPlugin] Cannot get player WFS delays");
        }
    }

    /// <summary>Retrieves WFS linear gain values for all output channels from the native library.</summary>
    public unsafe void getVolume(int id, float[] volume, int arraySize)
    {
        fixed (float* ptr = volume)
        {
            if (AT_WS_getPlayerWfsLinGain(id, (IntPtr)ptr, arraySize) == AUDIO_PLUGIN_ERROR)
                Debug.LogError("[AudioPlugin] Cannot get player WFS linear gain");
        }
    }

    /// <summary>Retrieves the speaker activation mask for this source from the native library.</summary>
    public unsafe void getSpeakerMask(int id, float[] speakerMask, int arraySize)
    {
        fixed (float* ptr = speakerMask)
        {
            if (AT_WS_getPlayerSpeakerMask(id, (IntPtr)ptr, arraySize) == AUDIO_PLUGIN_ERROR)
                Debug.LogError("[AudioPlugin] Cannot get player speaker mask");
        }
    }

    /// <summary>Retrieves RMS meter values for all audio file channels from the native library.</summary>
    public unsafe void getMeters(int id, float[] meters, int arraySize)
    {
        fixed (float* ptr = meters)
        {
            if (arraySize <= 0 || meters == null || meters.Length == 0) return;
            if (AT_WS_getPlayerMeters(id, (IntPtr)ptr, arraySize) == AUDIO_PLUGIN_ERROR)
                Debug.LogError("[AudioPlugin] Cannot get player meters");
        }
    }
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
        if (!is3D) return;

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
    #endregion
}
