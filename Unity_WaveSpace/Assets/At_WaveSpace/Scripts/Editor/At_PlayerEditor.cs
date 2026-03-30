/// @file At_PlayerEditor.cs
/// @brief Custom Inspector for At_Player.
///
/// @details
/// Handles persistent state load/save, file selection with fast metadata reading,
/// real-time metering, gain control, and playback parameter editing.
/// Inspector repaints are limited to active playback to minimize Editor overhead.

using System.Collections.Generic;
using System;
using UnityEngine;
using UnityEditor;
using System.IO;
using UnityEngine.SceneManagement;
using System.Runtime.InteropServices;

[ExecuteInEditMode]
[CanEditMultipleObjects]
[CustomEditor(typeof(At_Player))]
public class At_PlayerEditor : Editor
{
    #region Private Variables
    private At_Player      player;
    private At_MasterOutput masterOutput;
    private At_PlayerState  playerState;

    private Texture  meterOn;
    private Texture  meterOff;
    private GUIStyle horizontalLine;

    private bool   shouldSave      = false;
    private bool   isSceneLoading  = false;
    private bool   previousIsEditor;
    private bool   previousIsPlaying;

    // Throttle Inspector updates when not playing (max 10/s)
    private double lastUpdateTime           = 0;
    private const  double UPDATE_THROTTLE   = 0.1;

    // Engine initialization guard for Edit-mode metadata reads
    private static bool s_audioManagerInitialized = false;
    #endregion

    #region Serialized Properties
    private SerializedProperty sp_fileName;
    private SerializedProperty sp_gain;
    private SerializedProperty sp_is3D;
    private SerializedProperty sp_isPlayingOnAwake;
    private SerializedProperty sp_isLooping;
    private SerializedProperty sp_playbackSpeed;
    private SerializedProperty sp_numChannelsInAudioFile;
    private SerializedProperty sp_attenuation;
    private SerializedProperty sp_minDistance;
    private SerializedProperty sp_lowPassFc;
    private SerializedProperty sp_lowPassGain;
    private SerializedProperty sp_lowPassBypass;
    private SerializedProperty sp_highPassFc;
    private SerializedProperty sp_highPassGain;
    private SerializedProperty sp_highPassBypass;
    #endregion

    #region Initialization
    public void OnEnable()
    {
        SceneManager.sceneLoaded += OnSceneLoaded;

        sp_fileName              = serializedObject.FindProperty("fileName");
        sp_gain                  = serializedObject.FindProperty("gain");
        sp_is3D                  = serializedObject.FindProperty("is3D");
        sp_isLooping             = serializedObject.FindProperty("isLooping");
        sp_isPlayingOnAwake      = serializedObject.FindProperty("isPlayingOnAwake");
        sp_attenuation           = serializedObject.FindProperty("attenuation");
        sp_minDistance           = serializedObject.FindProperty("minDistance");
        sp_playbackSpeed         = serializedObject.FindProperty("playbackSpeed");
        sp_numChannelsInAudioFile = serializedObject.FindProperty("numChannelsInAudioFile");
        sp_lowPassFc             = serializedObject.FindProperty("lowPassFc");
        sp_lowPassGain           = serializedObject.FindProperty("lowPassGain");
        sp_highPassFc            = serializedObject.FindProperty("highPassFc");
        sp_highPassGain          = serializedObject.FindProperty("highPassGain");
        sp_lowPassBypass         = serializedObject.FindProperty("lowPassBypass");
        sp_highPassBypass        = serializedObject.FindProperty("highPassBypass");

        previousIsEditor  = Application.isEditor;
        previousIsPlaying = Application.isPlaying;

        player = (At_Player)target;

        if (player.isDynamicInstance) return;

        horizontalLine = new GUIStyle
        {
            normal      = { background = EditorGUIUtility.whiteTexture },
            margin      = new RectOffset(0, 0, 4, 4),
            fixedHeight = 1
        };

        string sceneName = SceneManager.GetActiveScene().name;
        playerState = At_AudioEngineUtils.getPlayerStateWithGuidAndName(sceneName, player.guid, player.gameObject.name);

        if (playerState == null)
        {
            playerState = At_AudioEngineUtils.createNewPlayerStateWithGuidAndName(sceneName, player.guid, player.gameObject.name);
            playerState.fileName               = sp_fileName.stringValue;
            playerState.gain                   = sp_gain.floatValue;
            playerState.attenuation            = sp_attenuation.floatValue;
            playerState.minDistance            = sp_minDistance.floatValue;
            playerState.numChannelsInAudiofile  = sp_numChannelsInAudioFile.intValue;
            playerState.is3D                   = true;
            playerState.isPlayingOnAwake       = true;
            playerState.isLooping              = true;
            playerState.playbackSpeed          = 1.0f;
            playerState.lowPassFc              = 20000.0f;
            playerState.highPassFc             = 20.0f;
            playerState.lowPassBypass          = true;
            playerState.highPassBypass         = false;
        }

        LoadParametersFromState();

        if (!Application.isPlaying)
            player.initMeters();

        At_AudioEngineUtils.SaveAllState(sceneName);
    }

    private void OnSceneLoaded(Scene scene, LoadSceneMode mode) => isSceneLoading = false;

    public void OnDisable()
    {
        if (player != null && playerState != null && player.name != playerState.name)
            At_AudioEngineUtils.changePlayerName(SceneManager.GetActiveScene().name, playerState.name, player.name);

        if (shouldSave && player != null && !player.isDynamicInstance)
        {
            At_AudioEngineUtils.SaveAllState(SceneManager.GetActiveScene().name);
            shouldSave = false;
        }

        At_AudioEngineUtils.CleanAllStates(SceneManager.GetActiveScene().name);
    }

    private void OnDestroy()
    {
        if (Application.isEditor != previousIsEditor || Application.isPlaying != previousIsPlaying) return;

        if (player == null && !isSceneLoading && playerState != null)
            At_AudioEngineUtils.removePlayerWithGuid(SceneManager.GetActiveScene().name, playerState.guid);

        isSceneLoading    = false;
        previousIsEditor  = Application.isEditor;
        previousIsPlaying = Application.isPlaying;
    }

    /// <summary>Only requests constant repaints while audio is playing.</summary>
    public override bool RequiresConstantRepaint() => player != null && player.isPlaying;

    private void AffirmResources()
    {
        if (meterOn != null) return;
        meterOn  = Resources.Load<Texture>("At_WaveSpace/LevelMeter");
        meterOff = Resources.Load<Texture>("At_WaveSpace/LevelMeterOff");
    }
    #endregion

    #region Inspector GUI
    public override void OnInspectorGUI()
    {
        if (player.isDynamicInstance)
        {
            EditorGUILayout.HelpBox("Dynamic player instance created at runtime.", MessageType.Info);
            return;
        }

        if (player.name != playerState.name)
            At_AudioEngineUtils.changePlayerName(SceneManager.GetActiveScene().name, playerState.name, player.name);

        AffirmResources();
        if (isSceneLoading) return;

        GUILayout.Space(10);

        if (!Application.isPlaying) DrawFileSelectionSection();
        if  (Application.isPlaying) DrawPlaybackControlSection();

        DrawMeteringSection();
        HorizontalLine(Color.black);

        if (!Application.isPlaying) DrawBasicParametersSection();

        DrawPlaybackSpeed();

        if (playerState.is3D) Draw3DParametersSection();

        UpdatePlayerParameters();
    }

    private void DrawFileSelectionSection()
    {
        EditorGUILayout.BeginHorizontal();
        if (GUILayout.Button("Open", GUILayout.Width(60)))
        {
            // Open browser directly in StreamingAssets/Audio, fallback to StreamingAssets
            string audioDir = System.IO.Path.Combine(Application.streamingAssetsPath, "Audio");
            if (!System.IO.Directory.Exists(audioDir))
                audioDir = Application.streamingAssetsPath;

            string absPath = EditorUtility.OpenFilePanel("Select Audio File", audioDir, "wav,mp3,ogg, m4a");
            if (!string.IsNullOrEmpty(absPath))
            {
                // Store path relative to StreamingAssets (portable Mac/Windows)
                string streamingRoot = Application.streamingAssetsPath.Replace('\\', '/');
                string normalizedAbs = absPath.Replace('\\', '/');
                string relativePath  = normalizedAbs.StartsWith(streamingRoot)
                    ? normalizedAbs.Substring(streamingRoot.Length).TrimStart('/')
                    : absPath;   // fallback: keep absolute if outside StreamingAssets

                // Full path passed to the native engine for file I/O
                string fullPath = System.IO.Path.Combine(Application.streamingAssetsPath, relativePath);

                playerState.fileName = relativePath;
                player.fileName      = fullPath;

                EnsureAudioManagerInitialized();

                int    numChannels;
                double sampleRate, lengthSeconds;
                long   totalSamples;

                if (AT_WS_getAudioFileMetadata(fullPath, out numChannels, out sampleRate, out lengthSeconds, out totalSamples) == 0)
                {
                    playerState.numChannelsInAudiofile = numChannels;
                    Debug.Log($"[At_PlayerEditor] {Path.GetFileName(fullPath)}: {numChannels}ch, {sampleRate:F0}Hz, {lengthSeconds:F2}s");
                }
                else
                {
                    Debug.LogError($"[At_PlayerEditor] Failed to read metadata for: {fullPath}. Defaulting to stereo.");
                    playerState.numChannelsInAudiofile = 2;
                }

                player.numChannelsInAudioFile = playerState.numChannelsInAudiofile;
                player.initMeters();

                serializedObject.Update();
                sp_fileName.stringValue              = relativePath;
                sp_numChannelsInAudioFile.intValue   = playerState.numChannelsInAudiofile;
                serializedObject.ApplyModifiedProperties();

                EditorUtility.SetDirty(player);
                shouldSave = true;
            }
        }
        // Display only the filename for readability, tooltip shows the full relative path
        string displayName = string.IsNullOrEmpty(playerState.fileName)
            ? ""
            : Path.GetFileName(playerState.fileName);
        // Label carries the tooltip (full relative path); TextField shows only the filename
        EditorGUILayout.TextField(new GUIContent(string.Empty, playerState.fileName), displayName);
        EditorGUILayout.EndHorizontal();
        GUILayout.Space(10);
    }

    /// <summary>
    /// Initializes the native audio manager once in Edit mode so that file metadata
    /// can be read without a full engine setup.
    /// </summary>
    private void EnsureAudioManagerInitialized()
    {
        if (s_audioManagerInitialized) return;
        try
        {
            if (AT_WS_initialize() == 0)
            {
                s_audioManagerInitialized = true;
                Debug.Log("[At_PlayerEditor] AudioManager initialized for Edit-mode metadata reading");
            }
        }
        catch (Exception ex)
        {
            Debug.LogError("[At_PlayerEditor] Exception initializing AudioManager: " + ex.Message);
        }
    }

    private void DrawPlaybackControlSection()
    {
        EditorGUILayout.BeginHorizontal();
        GUILayout.FlexibleSpace();
        if (player.isPlaying)
        {
            if (GUILayout.Button("■ Stop", GUILayout.Width(100), GUILayout.Height(30)))
                player.StopPlaying();
        }
        else
        {
            if (GUILayout.Button("▶ Play", GUILayout.Width(100), GUILayout.Height(30)))
                player.StartPlaying();
        }
        GUILayout.FlexibleSpace();
        EditorGUILayout.EndHorizontal();
        GUILayout.Space(5);
        HorizontalLine(Color.black);
        GUILayout.Space(5);
    }

    private void DrawMeteringSection()
    {
        GUILayout.Label("Gain", EditorStyles.boldLabel);

        EditorGUILayout.BeginHorizontal();
        GUILayout.Label("Gain (dB)", GUILayout.Width(150));
        string gainStr = EditorGUILayout.TextField(playerState.gain.ToString("F1"), GUILayout.Width(60));
        if (float.TryParse(gainStr,
                System.Globalization.NumberStyles.Float,
                System.Globalization.CultureInfo.InvariantCulture,
                out float pg))
        {
            pg = Mathf.Clamp(pg, -80f, 10f);
            if (!Mathf.Approximately(pg, playerState.gain)) { playerState.gain = pg; shouldSave = true; }
        }
        GUILayout.Label("dB", EditorStyles.miniLabel);
        EditorGUILayout.EndHorizontal();

        EditorGUILayout.BeginHorizontal();
        float newGain = GUILayout.HorizontalSlider(playerState.gain, -80f, 10f);
        if (!Mathf.Approximately(newGain, playerState.gain)) { playerState.gain = newGain; shouldSave = true; }
        EditorGUILayout.EndHorizontal();
        GUILayout.Space(15);
    }

    private void DrawBasicParametersSection()
    {
        EditorGUILayout.BeginHorizontal();
        GUILayout.Label("Spatialization Mode", GUILayout.Width(150));
        bool newIs3D = EditorGUILayout.Toggle(playerState.is3D, GUILayout.Width(20));
        if (newIs3D != playerState.is3D) { playerState.is3D = newIs3D; shouldSave = true; }
        GUILayout.Label(playerState.is3D ? "3D (WFS)" : "2D (Direct)", EditorStyles.miniLabel);
        EditorGUILayout.EndHorizontal();

        EditorGUILayout.BeginHorizontal();
        GUILayout.Label("Play On Awake", GUILayout.Width(150));
        bool newPOA = EditorGUILayout.Toggle(playerState.isPlayingOnAwake, GUILayout.Width(20));
        if (newPOA != playerState.isPlayingOnAwake) { playerState.isPlayingOnAwake = newPOA; shouldSave = true; }
        EditorGUILayout.EndHorizontal();

        EditorGUILayout.BeginHorizontal();
        GUILayout.Label("Loop", GUILayout.Width(150));
        bool newLoop = EditorGUILayout.Toggle(playerState.isLooping, GUILayout.Width(20));
        if (newLoop != playerState.isLooping) { playerState.isLooping = newLoop; shouldSave = true; }
        EditorGUILayout.EndHorizontal();

        HorizontalLine(Color.black);
    }


    private void DrawPlaybackSpeed()
    {
        EditorGUILayout.BeginHorizontal();
        GUILayout.Label("Playback Speed", GUILayout.Width(150));
        string speedStr = EditorGUILayout.TextField(playerState.playbackSpeed.ToString("F2"), GUILayout.Width(60));
        if (float.TryParse(speedStr, out float ps))
        {
            ps = Mathf.Clamp(ps, 0.1f, 4.0f);
            if (ps != playerState.playbackSpeed) { playerState.playbackSpeed = ps; shouldSave = true; }
        }
        EditorGUILayout.EndHorizontal();

        EditorGUILayout.BeginHorizontal();
        float newSpd = GUILayout.HorizontalSlider(playerState.playbackSpeed, 0.1f, 4.0f);
        if (newSpd != playerState.playbackSpeed) { playerState.playbackSpeed = newSpd; shouldSave = true; }
        EditorGUILayout.EndHorizontal();
        GUILayout.Space(15);
    }

    private void Draw3DParametersSection()
    {
        HorizontalLine(Color.black);

        GUILayout.Space(5);

        EditorGUILayout.BeginHorizontal();
        GUILayout.Label("Minimum Distance (m)", GUILayout.Width(150));
        string minDistStr = EditorGUILayout.TextField(playerState.minDistance.ToString("F1"), GUILayout.Width(60));
        if (float.TryParse(minDistStr, out float pd)) { pd = Mathf.Clamp(pd, 0.1f, 50f); if (pd != playerState.minDistance) { playerState.minDistance = pd; shouldSave = true; } }
        EditorGUILayout.EndHorizontal();

        EditorGUILayout.BeginHorizontal();
        float newDist = GUILayout.HorizontalSlider(playerState.minDistance, 0.1f, 50f);
        if (newDist != playerState.minDistance) { playerState.minDistance = newDist; shouldSave = true; }
        EditorGUILayout.EndHorizontal();
        GUILayout.Space(15);

        EditorGUILayout.BeginHorizontal();
        GUILayout.Label("Attenuation", GUILayout.Width(150));
        string attenStr = EditorGUILayout.TextField(playerState.attenuation.ToString("F2"), GUILayout.Width(60));
        if (float.TryParse(attenStr, out float pa)) { pa = Mathf.Clamp(pa, 0f, 2f); if (pa != playerState.attenuation) { playerState.attenuation = pa; shouldSave = true; } }
        EditorGUILayout.EndHorizontal();

        EditorGUILayout.BeginHorizontal();
        float newAtten = GUILayout.HorizontalSlider(playerState.attenuation, 0f, 2f);
        if (newAtten != playerState.attenuation) { playerState.attenuation = newAtten; shouldSave = true; }
        EditorGUILayout.EndHorizontal();
        GUILayout.Space(15);
    }
    #endregion

    #region Parameter Synchronization
    private void LoadParametersFromState()
    {
        if (playerState == null) return;
        player.fileName               = playerState.fileName;
        player.gain                   = playerState.gain;
        player.is3D                   = playerState.is3D;
        player.isLooping              = playerState.isLooping;
        player.isPlayingOnAwake       = playerState.isPlayingOnAwake;
        player.attenuation            = playerState.attenuation;
        player.minDistance            = playerState.minDistance;
        player.playbackSpeed          = playerState.playbackSpeed;
        player.numChannelsInAudioFile = playerState.numChannelsInAudiofile;
        player.lowPassFc              = playerState.lowPassFc;
        player.lowPassGain            = playerState.lowPassGain;
        player.highPassFc             = playerState.highPassFc;
        player.highPassGain           = playerState.highPassGain;
        player.lowPassBypass          = playerState.lowPassBypass;
        player.highPassBypass         = playerState.highPassBypass;
    }

    /// <summary>
    /// Propagates state changes to the player component and serialized properties.
    /// Updates are throttled when not playing to reduce Editor overhead.
    /// </summary>
    private void UpdatePlayerParameters()
    {
        if (!player.isPlaying)
        {
            double now = EditorApplication.timeSinceStartup;
            if (now - lastUpdateTime < UPDATE_THROTTLE) return;
            lastUpdateTime = now;
        }

        bool changed = false;
        void Sync<T>(ref T playerVal, T stateVal) where T : System.IEquatable<T>
            { if (!playerVal.Equals(stateVal)) { playerVal = stateVal; changed = true; } }

        Sync(ref player.gain,                   playerState.gain);
        Sync(ref player.is3D,                   playerState.is3D);
        Sync(ref player.isPlayingOnAwake,        playerState.isPlayingOnAwake);
        Sync(ref player.fileName,               playerState.fileName);
        Sync(ref player.isLooping,              playerState.isLooping);
        Sync(ref player.attenuation,            playerState.attenuation);
        Sync(ref player.minDistance,            playerState.minDistance);
        Sync(ref player.playbackSpeed,          playerState.playbackSpeed);
        Sync(ref player.numChannelsInAudioFile, playerState.numChannelsInAudiofile);
        Sync(ref player.lowPassFc,              playerState.lowPassFc);
        Sync(ref player.lowPassGain,            playerState.lowPassGain);
        Sync(ref player.highPassFc,             playerState.highPassFc);
        Sync(ref player.highPassGain,           playerState.highPassGain);
        Sync(ref player.lowPassBypass,          playerState.lowPassBypass);
        Sync(ref player.highPassBypass,         playerState.highPassBypass);

        if (!changed) return;

        serializedObject.Update();
        sp_gain.floatValue                   = playerState.gain;
        sp_is3D.boolValue                    = playerState.is3D;
        sp_isPlayingOnAwake.boolValue        = playerState.isPlayingOnAwake;
        sp_fileName.stringValue              = playerState.fileName;
        sp_isLooping.boolValue               = playerState.isLooping;
        sp_attenuation.floatValue            = playerState.attenuation;
        sp_minDistance.floatValue            = playerState.minDistance;
        sp_playbackSpeed.floatValue          = playerState.playbackSpeed;
        sp_numChannelsInAudioFile.intValue   = playerState.numChannelsInAudiofile;
        sp_lowPassFc.floatValue              = playerState.lowPassFc;
        sp_lowPassGain.floatValue            = playerState.lowPassGain;
        sp_highPassFc.floatValue             = playerState.highPassFc;
        sp_highPassGain.floatValue           = playerState.highPassGain;
        sp_lowPassBypass.boolValue           = playerState.lowPassBypass;
        sp_highPassBypass.boolValue          = playerState.highPassBypass;
        serializedObject.ApplyModifiedProperties();
    }
    #endregion

    #region Metering Display
    private void DisplayMeteringWithWidth(float[] metering, bool isPlaying, int numChannels, float maxWidth, int meterHeight)
    {
        if (meterOff == null || metering == null || metering.Length != numChannels) return;

        float meterWidth = Mathf.Clamp(maxWidth / numChannels, 8f, (128f / meterOff.height) * meterOff.width);
        float totalWidth = meterWidth * numChannels;

        Rect fullRect = GUILayoutUtility.GetRect(totalWidth, meterHeight,
            GUILayout.Width(totalWidth), GUILayout.Height(meterHeight));

        int[]   segmentPixels = { 0, 18, 38, 60, 89, 130, 187, 244, 300 };
        float[] segmentDB     = { -80f, -60f, -50f, -40f, -30f, -20f, -10f, 0f, 10f };

        for (int i = 0; i < numChannels; i++)
        {
            Rect meterRect = new Rect(fullRect.x + meterWidth * i, fullRect.y, meterWidth, fullRect.height);
            GUI.DrawTexture(meterRect, meterOff);

            float db = Mathf.Clamp(metering[i], -90f, 10f);
            int   seg = 1;
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

    private void HorizontalLine(Color color)
    {
        var c = GUI.color;
        GUI.color = color;
        GUILayout.Box(GUIContent.none, horizontalLine);
        GUI.color = c;
    }
    #endregion

    #region DLL Imports
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int AT_WS_initialize();
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int AT_WS_shutdown();
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int AT_WS_addPlayer(out int uid);
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int AT_WS_setPlayerFilePath(int uid, string path);
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int AT_WS_getPlayerNumChannel(int id, out int numChannel);
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int AT_WS_getAudioFileMetadata(string filepath, out int numChannels, out double sampleRate, out double lengthSeconds, out long totalSamples);
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)] private static extern int AT_WS_removePlayer(int uid);
    #endregion
}
