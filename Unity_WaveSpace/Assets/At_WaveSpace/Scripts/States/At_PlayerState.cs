/// @file At_PlayerState.cs
/// @brief Persistent player configuration saved to a JSON state file.

using UnityEngine;

public class At_PlayerState
{
    /// <summary>Player type identifier (reserved for future use).</summary>
    public int type = 0;

    /// <summary>Unique identifier matching the At_Player component GUID.</summary>
    public string guid;

    /// <summary>Name of the GameObject the At_Player is attached to.</summary>
    public string name = "";

    /// <summary>Path to the audio file to play (must be in StreamingAssets).</summary>
    public string fileName = "";

    /// <summary>Gain applied to the audio file (dB).</summary>
    public float gain = 0;

    /// <summary>True = 3D WFS spatialization; false = 2D direct output.</summary>
    public bool is3D = false;

    /// <summary>If true, playback starts automatically on Awake.</summary>
    public bool isPlayingOnAwake = false;

    /// <summary>If true, the audio file loops.</summary>
    public bool isLooping = false;

    /// <summary>Playback speed multiplier (1.0 = normal speed).</summary>
    public float playbackSpeed = 1.0f;

    /// <summary>Distance attenuation exponent for this source.</summary>
    public float attenuation = 2;

    /// <summary>Index of the selected attenuation type in the Inspector popup.</summary>
    public int selectedAttenuation = 0;

    /// <summary>Minimum distance below which no attenuation is applied (meters).</summary>
    public float minDistance = 1;

    /// <summary>Number of channels in the audio file.</summary>
    public int numChannelsInAudiofile = 1;

    /// <summary>Low-pass filter cutoff frequency (Hz).</summary>
    public float lowPassFc = 20000.0f;

    /// <summary>Low-pass filter gain (dB).</summary>
    public float lowPassGain = 0.0f;

    /// <summary>High-pass filter cutoff frequency (Hz).</summary>
    public float highPassFc = 20.0f;

    /// <summary>High-pass filter gain (dB).</summary>
    public float highPassGain = 0.0f;

    /// <summary>If true, the low-pass filter is bypassed.</summary>
    public bool lowPassBypass = true;

    /// <summary>If true, the high-pass filter is bypassed.</summary>
    public bool highPassBypass = true;

    /// <summary>
    /// If true, applies listener-position-dependent 6DOF source masking to
    /// this player's output (2D mode only — has no effect in 3D/WFS mode).
    /// See AT_SixDofMaskProcessor (native) for the algorithm.
    /// </summary>
    public bool is6dofMaskEnabled = false;

    /// <summary>
    /// Grid resolution (metres) for 6DOF source mode-detection. Default 0.02,
    /// validated on the clean synthetic FocalSources corpus — real/reverberant
    /// recordings typically need a coarser value (0.1-0.3).
    /// </summary>
    public float sixDofGridRes = 0.02f;

    /// <summary>
    /// Minimum number of matching position estimates (within the rolling
    /// localization history) for a 6DOF source to be accepted. Default 4
    /// (synthetic corpus).
    /// </summary>
    public int sixDofMinBlockCount = 4;

    /// <summary>
    /// Number of audio callback blocks accumulated into one 6DOF localization
    /// analysis window. 1 = lowest latency (default, try first).
    /// </summary>
    public int sixDofNumBufferedBlocks = 1;
}
