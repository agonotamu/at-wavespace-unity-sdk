/// @file At_PlayerState.cs
/// @brief Persistent player configuration saved to a JSON state file.

using UnityEngine;

[System.Serializable]
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
    /// Minimum ratio between a grid peak's pseudo-spectrum value and the
    /// window's own noise-floor estimate (median pseudo-spectrum value
    /// across the whole search grid) for that peak to be accepted as a real
    /// source — evaluated fresh within each analysis window, no
    /// cross-window history (detection is instantaneous, follows moving or
    /// intermittent sources). Higher = stricter (fewer false positives, may
    /// miss weak/distant sources); lower = more permissive. Needs empirical
    /// tuning against real recordings — default 10 is a first guess, not a
    /// validated constant.
    /// </summary>
    public float sixDofConfidenceThreshold = 10.0f;

    /// <summary>
    /// Number of audio callback blocks accumulated into one 6DOF localization
    /// analysis window. Must be large enough to yield several STFT snapshots
    /// per analyzed frequency band for a numerically well-behaved covariance
    /// estimate. Default 8 (not 1 — MUSIC needs more than one small callback
    /// block's worth of samples, unlike the previous GCC-PHAT implementation).
    /// </summary>
    public int sixDofNumBufferedBlocks = 8;

    /// <summary>
    /// Number of sources the MUSIC signal subspace is sized for, and the
    /// maximum number of pseudo-spectrum peaks searched per analysis window.
    /// Generous values cost little (validated: negligible timing impact) —
    /// the true source count is resolved downstream by the temporal
    /// mode/hysteresis history above, not by tuning this precisely.
    /// Default 3.
    /// </summary>
    public int sixDofMaxSources = 3;

    /// <summary>
    /// Spatial resolution (metres) of the MUSIC candidate-position search
    /// grid. Dominant cost lever — cost scales ~1/resolution^2; coarser than
    /// the masking transition width (0.3 m) buys little accuracy. Default 0.5.
    /// </summary>
    public float sixDofSearchGridResolution = 0.5f;

    /// <summary>
    /// Number of frequency bands combined ("incoherent combination") per
    /// analysis window, spread linearly across [sixDofBandHzMin, sixDofBandHzMax].
    /// Cost scales ~linearly with this value. Default 8.
    /// </summary>
    public int sixDofMaxBins = 8;

    /// <summary>Lower bound (Hz) of the frequency range analyzed by MUSIC. Default 400.</summary>
    public float sixDofBandHzMin = 400.0f;

    /// <summary>Upper bound (Hz) of the frequency range analyzed by MUSIC. Default 4000.</summary>
    public float sixDofBandHzMax = 4000.0f;

    /// <summary>Lower bound (metres) of the height range swept by the MUSIC search grid. Default -1.</summary>
    public float sixDofYRangeMin = -1.0f;

    /// <summary>Upper bound (metres) of the height range swept by the MUSIC search grid. Default 2.</summary>
    public float sixDofYRangeMax = 2.0f;
}
