/// @file At_OutputState.cs
/// @brief Persistent output configuration saved to a JSON state file.

using UnityEngine;

public class At_OutputState
{
    /// <summary>Name of the selected audio device.</summary>
    public string audioDeviceName = "select";

    /// <summary>Number of output channels used for the output bus.</summary>
    public int outputChannelCount = 0;

    /// <summary>Master gain for the output bus (dB).</summary>
    public float gain;

    /// <summary>Makeup gain applied after spatialization (dB).</summary>
    public float makeupGain;

    /// <summary>Index of the selected speaker configuration preset.</summary>
    public int selectSpeakerConfig = 0;

    /// <summary>Speaker rig geometry: 1=Linear, 2=Square, 3=Square 3/4, 4=Circle, 5=Circle 1/2.</summary>
    public int outputConfigDimension = 0;

    /// <summary>Index of the selected sampling rate in the Inspector popup.</summary>
    public int selectedSamplingRate = 0;

    /// <summary>Audio sampling rate (Hz).</summary>
    public int samplingRate = 48000;

    /// <summary>If true, WFS output channels are virtualized to stereo via HRTF.</summary>
    public bool isBinauralVirtualization = false;

    /// <summary>If true, bypasses WFS and applies HRTF directly to each source (A/B test mode).</summary>
    public bool isSimpleBinauralSpat = false;

    /// <summary>If true, applies a near-field IIR correction to the binaural virtualization pipeline.</summary>
    public bool isNearFieldCorrection = false;

    /// <summary>
    /// Distance (m) at which the loaded HRTF/BRIR dataset was measured.
    /// Used as the reference distance for the near-field ILD correction filter.
    /// Typically 1.0 m for a standard close-field HRTF set.
    /// </summary>
    public float hrtfDistance = 1.0f;

    /// <summary>Index of the selected buffer size in the Inspector popup.</summary>
    public int selectedBufferSize = 0;

    /// <summary>Audio buffer size in samples.</summary>
    public int bufferSize = 512;

    /// <summary>If true, the spatialization engine starts automatically on Awake.</summary>
    public bool isStartingEngineOnAwake = true;

    /// <summary>Diameter of the virtual speaker rig in meters (1 unit = 1 m).</summary>
    public float virtualSpeakerRigSize = 3;

    /// <summary>Maximum player-to-speaker distance used to size the WFS delay ring buffer.</summary>
    public float maxDistanceForDelay = 10.0f;

    /// <summary>Path to the HRTF data file (.txt, converted from SOFA format).</summary>
    public string hrtfFilePath = "";

    /// <summary>
    /// If true, HRTF impulse responses are truncated to 512 samples before being
    /// loaded into the convolution engine. Reduces CPU load at the cost of some
    /// low-frequency IR accuracy. Useful at high virtual-speaker counts (≥ 64).
    /// </summary>
    public bool isHrtfTruncated = false;

    /// <summary>Number of WFS virtual speakers (output channels).</summary>
    public int numVirtualSpeakers = 2;

    // =========================================================================
    // Global player settings — applied to ALL players, serialised here so
    // they belong to the output/engine configuration (not per-player state).
    // =========================================================================

    /// <summary>Global: enable WFS output mask for all players.</summary>
    public bool isWfsSpeakerMask = true;

    /// <summary>Global: enable WFS pre-filtering (2.5D half-derivative) for all players.</summary>
    public bool isPrefilter = false;

    /// <summary>
    /// Global: enable per-speaker WFS amplitude weighting cos(phi)/sqrt(r) for all players.
    /// False = delay-only mode.
    /// </summary>
    public bool isWfsGain = false;

    /// <summary>
    /// Global: restrict focused-source time-reversal min/max delay reference to
    /// active (non-masked) speakers only. Prevents L/R inversion when the WFS
    /// output mask is active.
    /// </summary>
    public bool isActiveSpeakersMinMax = false;

    /// <summary>
    /// Global: effective source radius (metres) for WFS singularity regularisation.
    ///
    /// In WFS, secondary sources are the physical loudspeakers of the array.
    /// Modelling them as finite-size sources (radius ε) regularises two
    /// singularities at the array-plane crossing:
    ///   P1 — Amplitude: r_eff = sqrt(r² + ε²) prevents cos(φ)/sqrt(r) → ∞
    ///        when a primary source reaches the plane of the array.
    ///        The delay path uses r_raw so wavefront phase is preserved.
    ///   P2 — Mask taper: replaces the hard speaker-activation gate with a C¹
    ///        raised-cosine ramp over ±ε, eliminating the silence step at
    ///        the non-focused ↔ focused transition.
    ///
    /// 0 = ideal point sources (original behaviour). Default: 0.3 m.
    /// Same value forwarded to the visual shader (_secondarySourceSize uniform).
    /// </summary>
    public float secondarySourceSize = 0.3f;

    /// <summary>Returns true when two states share the same device name and channel count.</summary>
    static public bool Compare(At_OutputState s1, At_OutputState s2)
    {
        return s1.audioDeviceName == s2.audioDeviceName
            && s1.outputChannelCount == s2.outputChannelCount;
    }
}
