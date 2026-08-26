/// @file At_RecordingWindow.cs
/// @brief Editor window for real-time recording of the engine's own audio output.
///
/// @details
/// Lets the user choose which internal bus to tap (2-channel downmix, raw
/// multichannel WFS bus, or a free-field capture-simulation of the raw bus —
/// see At_MasterOutput.RecordingSource), the output format (WAV or Ogg
/// Vorbis), and the destination file, then start/stop a real-time recording
/// while the engine is running (Play Mode).
///
/// Real-time only: there is no offline/faster-than-real-time rendering path
/// — see AT::SpatializationEngine::startRecording() (native) for why. The
/// Start button is therefore only enabled in Play Mode.

using UnityEngine;
using UnityEditor;

public class At_RecordingWindow : EditorWindow
{
    #region Constants
    private const double REPAINT_INTERVAL = 0.2; // 5 fps -- only used while a recording is in progress
    #endregion

    #region Private State
    private At_MasterOutput masterOutput;
    private At_MasterOutput.RecordingSource source = At_MasterOutput.RecordingSource.RawMultichannel;
    private At_MasterOutput.RecordingFormat format  = At_MasterOutput.RecordingFormat.Wav;
    private int    wavBitDepth   = 24;
    private int    vorbisQuality = 5; // 0-10
    private string filePath      = "";
    private double lastRepaintTime;
    #endregion

    #region Menu Item
    [MenuItem("AT_WaveSpace/Recording")]
    public static void ShowWindow()
    {
        At_RecordingWindow window = GetWindow<At_RecordingWindow>("Recording");
        window.minSize = new Vector2(320, 260);
        window.Show();
    }
    #endregion

    #region Unity Lifecycle
    private void OnEnable()
    {
        FindMasterOutput();
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
        if (masterOutput == null) FindMasterOutput();
        if (masterOutput == null || !masterOutput.isRecording) return;

        if (EditorApplication.timeSinceStartup - lastRepaintTime > REPAINT_INTERVAL)
        {
            masterOutput.RefreshIsRecording();
            Repaint();
            lastRepaintTime = EditorApplication.timeSinceStartup;
        }
    }

    private void OnPlayModeStateChanged(PlayModeStateChange state)
    {
        // Leaving Play Mode tears down the native engine — stop first so we
        // never leave isRecording stuck true or a file half-flushed.
        if (state == PlayModeStateChange.ExitingPlayMode
            && masterOutput != null && masterOutput.isRecording)
        {
            masterOutput.StopRecording();
        }

        if (state == PlayModeStateChange.EnteredEditMode || state == PlayModeStateChange.EnteredPlayMode)
            FindMasterOutput();
    }

    private void FindMasterOutput()
    {
        masterOutput = FindObjectOfType<At_MasterOutput>();
    }
    #endregion

    #region GUI Drawing
    private void OnGUI()
    {
        if (masterOutput == null) FindMasterOutput();

        GUILayout.Space(5);

        if (masterOutput == null)
        {
            EditorGUILayout.HelpBox("No At_MasterOutput found in the scene.", MessageType.Warning);
            return;
        }

        bool isRecordingNow = masterOutput.isRecording;

        using (new EditorGUI.DisabledScope(isRecordingNow))
        {
            DrawSourceSection();
            GUILayout.Space(8);
            DrawFormatSection();
            GUILayout.Space(8);
            DrawFileSection();
        }

        GUILayout.Space(10);
        DrawStartStopSection(isRecordingNow);
    }

    private void DrawSourceSection()
    {
        GUILayout.Label("Signal", EditorStyles.boldLabel);
        source = (At_MasterOutput.RecordingSource) EditorGUILayout.EnumPopup("Source", source);

        string help;
        switch (source)
        {
            case At_MasterOutput.RecordingSource.Downmix:
                help = "Final 2-channel bus (HRTF or amplitude-panning downmix). Requires "
                     + "\"Binaural Virtualization\" to be enabled in Master Output.";
                break;
            case At_MasterOutput.RecordingSource.RawMultichannel:
                help = "Raw WFS multichannel bus (one channel per virtual speaker), before any "
                     + "downmix and before master gain — independent of the physical device's own "
                     + "channel count.";
                break;
            default: // SimulatedCapture
                help = "Same raw multichannel bus as above, but every 3D player's WFS driving "
                     + "function is temporarily switched to a free-field model (1/r gain, pure "
                     + "propagation delay) matching simulate_capture.py — for generating "
                     + "GCC-PHAT/MUSIC corpus. Restored automatically when recording stops.";
                break;
        }
        EditorGUILayout.HelpBox(help, MessageType.None);

        if (source == At_MasterOutput.RecordingSource.Downmix && !masterOutput.isBinauralVirtualization)
            EditorGUILayout.HelpBox(
                "Binaural Virtualization is currently disabled — starting this recording will fail.",
                MessageType.Warning);
    }

    private void DrawFormatSection()
    {
        GUILayout.Label("Format", EditorStyles.boldLabel);
        format = (At_MasterOutput.RecordingFormat) EditorGUILayout.EnumPopup("Format", format);

        if (format == At_MasterOutput.RecordingFormat.Wav)
        {
            wavBitDepth = EditorGUILayout.IntPopup("Bit depth", wavBitDepth,
                new[] { "16", "24", "32" }, new[] { 16, 24, 32 });
        }
        else
        {
            vorbisQuality = EditorGUILayout.IntSlider("Quality", vorbisQuality, 0, 10);
        }
    }

    private void DrawFileSection()
    {
        GUILayout.Label("Destination", EditorStyles.boldLabel);
        EditorGUILayout.BeginHorizontal();
        filePath = EditorGUILayout.TextField(filePath);
        if (GUILayout.Button("...", GUILayout.Width(30)))
        {
            string ext = format == At_MasterOutput.RecordingFormat.Wav ? "wav" : "ogg";
            string startDir = string.IsNullOrEmpty(filePath)
                ? Application.dataPath
                : System.IO.Path.GetDirectoryName(filePath);
            string defaultName = string.IsNullOrEmpty(filePath)
                ? "recording." + ext
                : System.IO.Path.GetFileName(filePath);

            string chosen = EditorUtility.SaveFilePanel("Recording destination", startDir, defaultName, ext);
            if (!string.IsNullOrEmpty(chosen))
                filePath = chosen;
        }
        EditorGUILayout.EndHorizontal();
    }

    private void DrawStartStopSection(bool isRecordingNow)
    {
        if (!EditorApplication.isPlaying)
            EditorGUILayout.HelpBox("Real-time recording — enter Play Mode to start.", MessageType.Info);

        bool canStart = EditorApplication.isPlaying && !string.IsNullOrEmpty(filePath);
        using (new EditorGUI.DisabledScope(isRecordingNow ? !EditorApplication.isPlaying : !canStart))
        {
            if (!isRecordingNow)
            {
                if (GUILayout.Button("● Start Recording", GUILayout.Height(28)))
                {
                    int param = format == At_MasterOutput.RecordingFormat.Wav ? wavBitDepth : vorbisQuality;
                    masterOutput.StartRecording(filePath, source, format, param);
                }
            }
            else
            {
                if (GUILayout.Button("■ Stop Recording", GUILayout.Height(28)))
                    masterOutput.StopRecording();
            }
        }

        if (isRecordingNow)
        {
            GUILayout.Space(6);
            long   samples = masterOutput.GetRecordingSamplesWritten();
            double seconds = masterOutput.samplingRate > 0 ? (double) samples / masterOutput.samplingRate : 0.0;
            EditorGUILayout.LabelField("Recording", System.IO.Path.GetFileName(filePath));
            EditorGUILayout.LabelField("Elapsed", $"{seconds:F1} s  ({samples} samples)");
        }
    }
    #endregion
}
