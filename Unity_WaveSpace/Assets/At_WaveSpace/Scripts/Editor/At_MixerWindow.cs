/// @file At_MixerWindow.cs
/// @brief Centralized mixer window showing all audio players and the master output.
///
/// @details
/// Provides a compact strip view of every At_Player and the At_MasterOutput:
/// per-channel VU meters, a gain slider, play/stop buttons, and filename display.
/// The window updates at ≤10 fps to limit Editor CPU usage.

using UnityEngine;
using UnityEditor;
using UnityEngine.SceneManagement;
using System.Collections.Generic;

public class At_MixerWindow : EditorWindow
{
    #region Constants
    private const int   METER_HEIGHT      = 80;
    private const int   SLIDER_WIDTH      = 25;
    private const float MAX_METER_WIDTH   = 5f;
    private const float MIN_METER_WIDTH   = 3f;
    private const int   STRIP_WIDTH       = 50;
    private const int   MAX_FILENAME_LEN  = 7;
    private const int   MAX_NAME_LEN      = 7;
    private const double REPAINT_INTERVAL = 0.1; // 10 fps
    #endregion

    #region Private State
    private Vector2 scrollPosition;
    private Texture meterOn;
    private Texture meterOff;
    private At_MasterOutput masterOutput;
    private List<At_Player> players = new List<At_Player>();
    private At_OutputState  outputState;
    private Dictionary<string, At_PlayerState> playerStates = new Dictionary<string, At_PlayerState>();
    private double lastRepaintTime = 0;
    #endregion

    #region Menu Item
    [MenuItem("AT_WaveSpace/Mixer")]
    public static void ShowWindow()
    {
        At_MixerWindow window = GetWindow<At_MixerWindow>("Audio Mixer");
        window.minSize = new Vector2(100, 150);
        window.Show();
    }
    #endregion

    #region Unity Lifecycle
    private void OnEnable()
    {
        LoadResources();
        RefreshAudioSources();
        EditorApplication.playModeStateChanged += OnPlayModeStateChanged;
        EditorApplication.update              += OnEditorUpdate;
    }

    private void OnDisable()
    {
        EditorApplication.playModeStateChanged -= OnPlayModeStateChanged;
        EditorApplication.update              -= OnEditorUpdate;
    }

    private void OnEditorUpdate()
    {
        CleanupDestroyedObjects();

        if (EditorApplication.timeSinceStartup - lastRepaintTime > REPAINT_INTERVAL)
        {
            Repaint();
            lastRepaintTime = EditorApplication.timeSinceStartup;
        }
    }

    private void OnPlayModeStateChanged(PlayModeStateChange state)
    {
        if (state == PlayModeStateChange.EnteredEditMode || state == PlayModeStateChange.EnteredPlayMode)
            RefreshAudioSources();
    }
    #endregion

    #region Resource Loading
    private void LoadResources()
    {
        if (meterOn != null) return;
        meterOn  = Resources.Load<Texture>("At_WaveSpace/LevelMeter");
        meterOff = Resources.Load<Texture>("At_WaveSpace/LevelMeterOff");
    }
    #endregion

    #region Scene Object Management
    private void RefreshAudioSources()
    {
        masterOutput = FindObjectOfType<At_MasterOutput>();
        if (masterOutput != null)
            outputState = At_AudioEngineUtils.getOutputState(SceneManager.GetActiveScene().name);

        players.Clear();
        playerStates.Clear();

        foreach (At_Player p in FindObjectsOfType<At_Player>())
        {
            if (p.isDynamicInstance) continue;
            players.Add(p);
            At_PlayerState state = At_AudioEngineUtils.getPlayerStateWithGuidAndName(
                SceneManager.GetActiveScene().name, p.guid, p.gameObject.name);
            if (state != null) playerStates[p.guid] = state;
        }
    }

    private void CleanupDestroyedObjects()
    {
        players.RemoveAll(p => p == null);

        List<string> toRemove = new List<string>();
        foreach (var kvp in playerStates)
            if (!players.Exists(p => p != null && p.guid == kvp.Key))
                toRemove.Add(kvp.Key);
        foreach (string g in toRemove) playerStates.Remove(g);

        if (masterOutput == null)
        {
            masterOutput = FindObjectOfType<At_MasterOutput>();
            if (masterOutput != null)
                outputState = At_AudioEngineUtils.getOutputState(SceneManager.GetActiveScene().name);
        }
    }
    #endregion

    #region GUI Drawing
    private void OnGUI()
    {
        if (meterOn == null || meterOff == null) LoadResources();
        CleanupDestroyedObjects();
        DrawToolbar();

        scrollPosition = EditorGUILayout.BeginScrollView(scrollPosition);
        EditorGUILayout.BeginHorizontal();

        if (masterOutput != null && outputState != null)
        {
            DrawMakeupGainStrip();
            GUILayout.Space(2);
            DrawMasterOutputStrip();
            GUILayout.Space(2);
        }

        foreach (At_Player player in players)
        {
            if (player != null && playerStates.ContainsKey(player.guid))
            {
                DrawPlayerStrip(player, playerStates[player.guid]);
                GUILayout.Space(2);
            }
        }

        EditorGUILayout.EndHorizontal();
        EditorGUILayout.EndScrollView();
    }

    private void DrawToolbar()
    {
        EditorGUILayout.BeginHorizontal(EditorStyles.toolbar);
        if (GUILayout.Button("↻", EditorStyles.toolbarButton, GUILayout.Width(20)))
            RefreshAudioSources();
        GUILayout.FlexibleSpace();
        GUILayout.Label($"{players.Count}P", EditorStyles.miniLabel);
        EditorGUILayout.EndHorizontal();
    }

    private void DrawMasterOutputStrip()
    {
        if (masterOutput == null || outputState == null) return;

        EditorGUILayout.BeginVertical(GUI.skin.box, GUILayout.Width(STRIP_WIDTH));
        GUILayout.Label("MST", EditorStyles.miniLabel);

        DrawMetersAndGain(
            masterOutput.meters, masterOutput.isPlaying, masterOutput.outputChannelCount,
            outputState.gain, newGain =>
            {
                outputState.gain = newGain;
                masterOutput.gain = newGain;
                At_AudioEngineUtils.SaveAllState(SceneManager.GetActiveScene().name);
            });

        GUILayout.Label($"{masterOutput.outputChannelCount}ch", EditorStyles.miniLabel);
        EditorGUILayout.EndVertical();
    }

    private void DrawMakeupGainStrip()
    {
        if (masterOutput == null || outputState == null) return;

        EditorGUILayout.BeginVertical(GUI.skin.box, GUILayout.Width(STRIP_WIDTH));
        GUILayout.Label("MkUp", EditorStyles.miniLabel);

        float newMg = GUILayout.VerticalSlider(
            masterOutput.makeupGain, 40f, -10f,
            GUILayout.Height(METER_HEIGHT), GUILayout.Width(18));

        if (!Mathf.Approximately(newMg, masterOutput.makeupGain))
        {
            masterOutput.makeupGain = newMg;
            outputState.makeupGain  = newMg;
            At_AudioEngineUtils.SaveAllState(SceneManager.GetActiveScene().name);
        }

        GUILayout.Label($"{(int)masterOutput.makeupGain}", EditorStyles.miniLabel);
        EditorGUILayout.EndVertical();
    }

    private void DrawPlayerStrip(At_Player player, At_PlayerState state)
    {
        if (player == null || state == null) return;

        EditorGUILayout.BeginVertical(GUI.skin.box, GUILayout.Width(STRIP_WIDTH));

        string displayName = player.gameObject.name;
        if (displayName.Length > MAX_NAME_LEN)
            displayName = displayName.Substring(0, MAX_NAME_LEN - 1) + ".";

        if (GUILayout.Button(displayName, EditorStyles.miniLabel, GUILayout.Height(14)))
        {
            Selection.activeGameObject = player.gameObject;
            EditorGUIUtility.PingObject(player.gameObject);
        }

        if (player.numChannelsInAudioFile > 0 &&
            (player.meters == null || player.meters.Length != player.numChannelsInAudioFile))
            player.initMeters();

        DrawMetersAndGain(
            player.meters, player.isPlaying, player.numChannelsInAudioFile,
            state.gain, newGain =>
            {
                state.gain  = newGain;
                player.gain = newGain;
                At_AudioEngineUtils.SaveAllState(SceneManager.GetActiveScene().name);
            });

        if (!string.IsNullOrEmpty(state.fileName))
        {
            string fn = System.IO.Path.GetFileNameWithoutExtension(state.fileName);
            if (fn.Length > MAX_FILENAME_LEN) fn = fn.Substring(0, MAX_FILENAME_LEN - 1) + ".";
            GUILayout.Label(fn, EditorStyles.miniLabel);
        }
        GUILayout.Label($"{player.numChannelsInAudioFile}ch", EditorStyles.miniLabel);

        GUILayout.Space(2);
        if (player.isPlaying)
        {
            if (GUILayout.Button("■", EditorStyles.miniButton, GUILayout.Height(16))) player.StopPlaying();
        }
        else
        {
            if (GUILayout.Button("▶", EditorStyles.miniButton, GUILayout.Height(16))) player.StartPlaying();
        }

        EditorGUILayout.EndVertical();
    }

    private void DrawMetersAndGain(float[] meters, bool isPlaying, int numChannels,
        float currentGain, System.Action<float> onGainChanged)
    {
        EditorGUILayout.BeginHorizontal();

        if (numChannels <= 0 || meters == null || meters.Length != numChannels)
        {
            GUILayout.Label("---", EditorStyles.miniLabel, GUILayout.Height(METER_HEIGHT));
        }
        else
        {
            float meterWidth = Mathf.Max(MIN_METER_WIDTH,
                Mathf.Min(MAX_METER_WIDTH, (STRIP_WIDTH - SLIDER_WIDTH - 10f) / numChannels));
            DisplayMeters(meters, isPlaying, numChannels, meterWidth, METER_HEIGHT);
            GUILayout.Space(2);
        }

        EditorGUILayout.BeginVertical(GUILayout.Width(SLIDER_WIDTH));
        float newGain = GUILayout.VerticalSlider(currentGain, 10f, -80f,
            GUILayout.Height(METER_HEIGHT), GUILayout.Width(18));
        if (newGain != currentGain && onGainChanged != null)
        {
            try { onGainChanged(newGain); }
            catch { }
        }
        GUILayout.Label($"{(int)currentGain}", EditorStyles.miniLabel);
        EditorGUILayout.EndVertical();

        EditorGUILayout.EndHorizontal();
    }

    private void DisplayMeters(float[] metering, bool isPlaying, int numChannels,
        float meterWidth, int meterHeight)
    {
        if (meterOff == null) return;

        float totalWidth = meterWidth * numChannels;
        Rect  fullRect   = GUILayoutUtility.GetRect(totalWidth, meterHeight,
            GUILayout.Width(totalWidth), GUILayout.Height(meterHeight));

        int[]   segmentPixels = { 0, 18, 38, 60, 89, 130, 187, 244, 300 };
        float[] segmentDB     = { -80f, -60f, -50f, -40f, -30f, -20f, -10f, 0f, 10f };

        for (int i = 0; i < numChannels; i++)
        {
            Rect meterRect = new Rect(fullRect.x + meterWidth * i, fullRect.y, meterWidth, fullRect.height);
            GUI.DrawTexture(meterRect, meterOff);

            float db = Mathf.Clamp(i < metering.Length ? metering[i] : -80f, -90f, 10f);
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
    #endregion
}
