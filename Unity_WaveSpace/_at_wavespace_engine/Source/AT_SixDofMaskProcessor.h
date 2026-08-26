/**
 * @file AT_SixDofMaskProcessor.h
 * @brief Real-time listener-position-dependent per-channel masking for 6DOF
 *        navigation in dense multichannel captures (2D players whose N
 *        channels match the virtual speaker / real microphone array
 *        geometry, e.g. an equator/tropic ring extracted from the 1024-MEMS
 *        sphere).
 * @author Antoine Gonot / Claude
 * @date 2026
 *
 * Ported from the validated Python prototype (apply_mask_MUSIC.py,
 * FocalSources project):
 *   - Localization: MUSIC (Schmidt 1986) — per retained frequency band,
 *     eigendecomposition of the spatial covariance matrix splits a signal
 *     subspace (top `maxSources` eigenvectors) from a noise subspace; a
 *     candidate position's steering vector is (near-)orthogonal to the
 *     noise subspace iff it is a real source. Positions are tested on a
 *     regular spatial grid, combined ("incoherent combination") across
 *     several frequency bands. Chosen over GCC-PHAT (the previous
 *     implementation, removed) for robustness — MUSIC natively separates
 *     several simultaneously-active sources within a SINGLE analysis
 *     window, whereas GCC-PHAT needed many single-source-dominant windows
 *     accumulated over time. Costs more per window (eigendecomposition is
 *     O(mics^3) per band); mitigated by running on a background thread and
 *     keeping mics/bins/grid-resolution within the ranges validated for
 *     real-time use (see the 6dof-nav skill).
 *   - Eigendecomposition: hand-rolled real-symmetric Jacobi eigenvalue
 *     solver (no third-party linear algebra dependency), applied to the
 *     standard real 2N×2N block-matrix embedding of a complex N×N
 *     Hermitian matrix — see computeSignalSubspace()'s doc comment.
 *   - Multi-source detection: INSTANTANEOUS, evaluated fresh within each
 *     analysis window's own combined pseudo-spectrum — no cross-window
 *     history or repeat-detection requirement (replaces an earlier
 *     rolling-history/hysteresis design, removed: it assumed static
 *     sources revisited window after window, which broke down for
 *     intermittent or moving sources — see confidenceThreshold below).
 *     A grid peak is accepted as a real source this window iff its
 *     pseudo-spectrum value exceeds `confidenceThreshold` times the
 *     window's own noise-floor estimate (the median pseudo-spectrum value
 *     across the whole search grid — robust to the handful of genuine
 *     peaks, since real sources are always a small minority of grid
 *     points). A genuine source stands far above this floor (orders of
 *     magnitude, in validation); a "filler" peak MUSIC must still produce
 *     when fewer than `maxSources` sources are actually active barely
 *     clears it.
 *   - Masking: per-channel gain from the "source must lie between listener
 *     and microphone" validity criterion, combined (max) across all
 *     detected sources. No source separation/extraction is performed —
 *     the mask is applied directly to the captured signal already present
 *     in the player's buffer, which is sufficient for navigation (as
 *     opposed to per-source isolation/export, out of scope here).
 *
 * Dependencies: JUCE only (juce::dsp::FFT for the per-frame STFT, std:: for
 * threading/containers). No third-party numerical/linear-algebra library —
 * see computeSignalSubspace() for the in-house eigensolver this implies.
 *
 * Real-time safety: prepare()/releaseResources() allocate and are NOT
 * real-time safe (call from the same non-audio thread/contract as
 * SpatPlayer::setIs3D() already requires for m_puSpatializer). process()
 * IS real-time safe — no allocation, no locks held longer than a
 * try_lock-guarded pointer swap.
 */

#pragma once

#include <JuceHeader.h>
#include <vector>
#include <array>
#include <complex>
#include <mutex>
#include <atomic>
#include <thread>
#include <condition_variable>
#include <memory>

namespace AT
{
    /// One detected source position (metres, engine/world frame).
    struct SixDofSourcePosition
    {
        float x = 0.0f, y = 0.0f, z = 0.0f;
    };

    /**
     * @class SixDofMaskProcessor
     * @brief Localizes active sources in a multichannel 2D-player buffer and
     *        applies a listener-position-dependent per-channel gain mask.
     *
     * One instance per SpatPlayer (2D mode only, created lazily when the
     * "6DOF Source Masking" option is enabled — mirrors the m_puSpatializer
     * lazy-construction pattern for 3D mode). Owns a background thread for
     * localization (MUSIC is far too expensive to run on the audio thread);
     * the audio thread only ever reads a small mutex-guarded result and
     * applies cheap per-channel gains via FloatVectorOperations.
     */
    class SixDofMaskProcessor
    {
    public:
        SixDofMaskProcessor();
        ~SixDofMaskProcessor();

        /**
         * @brief Prepares the processor for a given channel/geometry configuration.
         * @param numChannels        Number of audio channels (= number of mics/virtual speakers)
         * @param sampleRate         Sample rate in Hz
         * @param maxBlockSize       Maximum audio callback block size (samples), up to 4096
         * @param micPositionsFlat   Flat [x0,y0,z0, x1,y1,z1, ...] array, numChannels*3 floats.
         *                           Copied internally; caller retains ownership of the source array.
         *
         * Must be called before process(). NOT real-time safe (allocates) —
         * call from the same thread/contract as SpatPlayer::setIs3D().
         */
        void prepare(int numChannels, double sampleRate, int maxBlockSize,
                     const float* micPositionsFlat);

        /// Stops the background thread and releases buffers. NOT real-time safe.
        void releaseResources();

        // --------------------------------------------------------------
        // Parameters — thread-safe (atomics), settable from any thread,
        // picked up by the audio/background thread on their next iteration.
        // --------------------------------------------------------------
        void setEnabled(bool enabled);
        bool isEnabled() const { return m_enabled.load(std::memory_order_relaxed); }

        /// Number of sources the MUSIC signal subspace is sized for, and the
        /// maximum number of pseudo-spectrum peaks searched per analysis
        /// window. Generous by design: unlike GCC-PHAT's num-sources
        /// parameter, this has negligible cost impact (validated: <32
        /// dimensions difference in the noise-subspace projection cost is
        /// noise-level in timing benchmarks) — the true source count is
        /// resolved downstream by the SAME temporal mode/hysteresis
        /// machinery as before (repeated peaks confirmed over time), not by
        /// tuning this value precisely. Default 3, capped at MAX_SOURCES.
        void setMaxSources(int maxSources);
        int getMaxSources() const { return m_maxSources.load(std::memory_order_relaxed); }

        /// Minimum ratio between a grid peak's pseudo-spectrum value and the
        /// window's own noise-floor estimate (median pseudo-spectrum value
        /// across the whole search grid) for that peak to be accepted as a
        /// real source THIS window — see the file doc comment above for the
        /// full rationale. Replaces the earlier grid-resolution-based
        /// temporal bucketing (removed) now that detection is instantaneous.
        /// Higher = stricter (fewer false positives, may miss weak/distant
        /// sources); lower = more permissive. Needs empirical tuning against
        /// real recordings — no single default suits every array/scene, so
        /// treat this starting value (10) as a first guess, not a validated
        /// constant.
        void setConfidenceThreshold(float confidenceThreshold);
        float getConfidenceThreshold() const { return m_confidenceThreshold.load(std::memory_order_relaxed); }

        /// Number of audio callback blocks accumulated into one localization
        /// analysis window. For MUSIC, this must be large enough to yield
        /// several STFT snapshots per retained frequency band — the spatial
        /// covariance estimate needs a comfortable snapshot count above the
        /// channel count to be numerically well-behaved. Default 8 (not 1 —
        /// unlike the previous GCC-PHAT implementation, a single small audio
        /// callback block rarely contains enough STFT frames for a stable
        /// covariance estimate).
        void setNumBufferedBlocks(int numBufferedBlocks);
        int getNumBufferedBlocks() const { return m_numBufferedBlocks.load(std::memory_order_relaxed); }

        /// Spatial resolution (metres) of the MUSIC candidate-position
        /// search grid. Dominant cost lever (validated: cost scales
        /// ~linearly with grid point count, i.e. ~1/resolution^2) — coarser
        /// than the masking transition width (0.3 m) buys little accuracy.
        /// Default 0.5.
        void setSearchGridResolution(float searchGridResolution);
        float getSearchGridResolution() const { return m_searchGridResolution.load(std::memory_order_relaxed); }

        /// Number of frequency bands combined ("incoherent combination")
        /// per analysis window, spread linearly across [bandHzMin, bandHzMax].
        /// Cost scales ~linearly with this value. Default 8.
        void setMaxBins(int maxBins);
        int getMaxBins() const { return m_maxBins.load(std::memory_order_relaxed); }

        /// Lower bound (Hz) of the analyzed frequency range. Default 400.
        void setBandHzMin(float bandHzMin);
        float getBandHzMin() const { return m_bandHzMin.load(std::memory_order_relaxed); }

        /// Upper bound (Hz) of the analyzed frequency range. Default 4000.
        void setBandHzMax(float bandHzMax);
        float getBandHzMax() const { return m_bandHzMax.load(std::memory_order_relaxed); }

        /// Lower bound (metres) of the height range swept by the search
        /// grid. Default -1.
        void setYRangeMin(float yRangeMin);
        float getYRangeMin() const { return m_yRangeMin.load(std::memory_order_relaxed); }

        /// Upper bound (metres) of the height range swept by the search
        /// grid. Default 2.
        void setYRangeMax(float yRangeMax);
        float getYRangeMax() const { return m_yRangeMax.load(std::memory_order_relaxed); }

        // --------------------------------------------------------------
        // Real-time audio-thread entry point
        // --------------------------------------------------------------

        // ----------------------------------------------------------------
        // Split into three steps (rather than one in-place process() call)
        // because the ANALYSIS must see the player's own un-mixed source
        // signal (before it is summed into the shared output buffer), while
        // the GAIN must be folded into SpatPlayer's existing per-sample
        // addWithMultiply loop (which already combines fade + distance gain
        // per channel) — multiplying the shared destination buffer in place
        // after the fact would incorrectly also attenuate whatever other
        // players already summed into the same channels.
        // ----------------------------------------------------------------

        /**
         * @brief Feeds `numSamples` samples of the player's OWN (un-mixed)
         *        source buffer into the rolling localization analysis
         *        window. Audio-thread, real-time safe, no allocation.
         *        No-op if disabled or not prepared.
         */
        void pushAnalysisBlock(const juce::AudioBuffer<float>& sourceBuffer, int startSample,
                                int numSamples, int numChannelsToProcess);

        /**
         * @brief Recomputes each channel's smoothed-gain TARGET from the
         *        latest detected source positions and the given listener
         *        position. Call once per block, audio-thread, before the
         *        per-sample loop that consumes getNextChannelGain().
         *        No-op if disabled or not prepared (existing smoother
         *        targets are left at their last value, i.e. hold — but
         *        callers should simply skip masking entirely when disabled,
         *        see isEnabled()).
         */
        void updateTargetGains(float listenerX, float listenerY, float listenerZ,
                                int numChannelsToProcess);

        /// Advances and returns the next smoothed gain sample for `channel`.
        /// Call exactly once per sample per channel, audio-thread, after
        /// updateTargetGains() for that block. Real-time safe, no allocation.
        float getNextChannelGain(int channel);

        // --------------------------------------------------------------
        // Introspection (for Unity display / debugging)
        // --------------------------------------------------------------

        /// Number of currently detected sources (0 if none / bypass state).
        int getNumDetectedSources() const;

        /**
         * @brief Copies detected source positions into a flat [x,y,z,...] array.
         * @param outPositions Destination buffer, at least maxSources*3 floats.
         * @param maxSources   Maximum number of sources to copy.
         * @return Number of sources actually copied.
         */
        int getDetectedSourcePositions(float* outPositions, int maxSources) const;

        /// True once the processor has fallen back to bypass (mask disabled,
        /// full pass-through gain) after repeated localization failures.
        /// See "Detection failure protection" in the .cpp — MAX_STALE_CYCLES.
        bool isInBypassFallback() const { return m_state.load(std::memory_order_relaxed) == State::Bypass; }

        static constexpr int MAX_SOURCES = 20;

    private:
        // --------------------------------------------------------------
        // Internal: localization (background thread)
        // --------------------------------------------------------------
        void backgroundThreadLoop();
        void runLocalizationOnWindow(const std::vector<std::vector<float>>& window);

        /**
         * @brief One spatial candidate on the MUSIC search grid.
         */
        struct GridPoint { float x, y, z; };

        /// Builds the regular candidate-position grid: a disc of radius
        /// equal to the array's own extent in the X/Z plane, swept over
        /// [yRangeMin, yRangeMax] in Y, at `resolution` spacing — direct
        /// port of build_grid() in apply_mask_MUSIC.py. NOT real-time safe
        /// (allocates); called once per analysis window on the background
        /// thread only.
        std::vector<GridPoint> buildSearchGrid(float resolution, float yMin, float yMax) const;

        /// Steering vector a(p,f): expected complex gain at each microphone
        /// for a hypothetical unit source at `pos` and frequency `freqHz`
        /// (propagation delay + 1/r decay, normalized to unit norm) — same
        /// physical model as simulate_capture.py / apply_mask_MUSIC.py's
        /// steering_vector(). Written into `outA` (must be m_numChannels long).
        void steeringVector(const GridPoint& pos, float freqHz, std::vector<std::complex<float>>& outA) const;

        /**
         * @brief Extracts the top `numSources` eigenvectors (signal
         * subspace) of a complex Hermitian covariance matrix K, without any
         * third-party linear algebra dependency.
         *
         * Method: a complex Hermitian N×N matrix K = Kre + i*Kim (Kre
         * symmetric, Kim antisymmetric) has the SAME eigenvalues as the
         * real symmetric 2N×2N block matrix
         *     M = [ Kre  -Kim ]
         *         [ Kim   Kre ]
         * each repeated twice; a real eigenvector (u;v) of M (u, v each
         * length N) for eigenvalue lambda corresponds to the complex
         * eigenvector w = u + i*v of K for that same lambda. M is
         * diagonalized with a hand-rolled cyclic Jacobi eigenvalue solver
         * (jacobiEigenSymmetric()) — simple, dependency-free, numerically
         * robust, adequate for the background-thread/periodic-update cost
         * budget validated for this feature (mics up to the low hundreds).
         * Only the top `numSources` eigenpairs of M (by eigenvalue) are
         * decoded back into complex vectors — taking one representative per
         * (numerically) degenerate pair, since MUSIC's projection test only
         * needs the SUBSPACE spanned, not a specific vector within a
         * degenerate pair.
         *
         * @param K          Complex Hermitian covariance, numChannels x numChannels, row-major.
         * @param numSources Number of leading eigenvectors to extract (signal subspace size).
         * @param outSignalSubspace Filled with `numSources` columns (each numChannels long).
         */
        void computeSignalSubspace(const std::vector<std::complex<float>>& K, int numSources,
                                    std::vector<std::vector<std::complex<float>>>& outSignalSubspace) const;

        /**
         * @brief Commits this window's accepted sources (already filtered by
         * the confidence test in runLocalizationOnWindow()) to
         * m_detectedSources, and drives the Valid/Stale/Bypass state machine
         * (see that enum's doc comment) — the only thing left that persists
         * ACROSS windows, and only for total-detection-failure recovery, not
         * per-source acceptance.
         */
        void finalizeDetectedSources(const std::vector<SixDofSourcePosition>& acceptedSources);

        // ---- Mask logging (debug, "on change" only) -----------------------
        // See updateTargetGains(): logs the per-source/per-channel mask
        // summary only when the listener position or detected source list
        // actually changed since the last log — avoids spamming the console
        // every audio block (updateTargetGains() runs far more often than
        // localization updates).
        bool m_maskLogInitialized = false;
        float m_lastLoggedListenerX = 0.0f, m_lastLoggedListenerY = 0.0f, m_lastLoggedListenerZ = 0.0f;
        std::vector<SixDofSourcePosition> m_lastLoggedSourcesForMaskLog;

        // --------------------------------------------------------------
        // Internal: masking (audio thread, cheap, no allocation)
        // --------------------------------------------------------------
        float computeChannelWeight(int channel, const SixDofSourcePosition& source,
                                    float lx, float ly, float lz) const;

        // --------------------------------------------------------------
        // Geometry (set once in prepare(), read-only afterwards — safe to
        // read from both threads without locking)
        // --------------------------------------------------------------
        int    m_numChannels = 0;
        double m_sampleRate  = 0.0;
        std::vector<std::array<float, 3>> m_micPositions;
        std::array<float, 3> m_arrayCentroid{ 0.0f, 0.0f, 0.0f };
        float  m_maxPlausibleDist  = 100.0f;   // 10x array extent, computed in prepare()
        float  m_arrayExtent       = 5.0f;     // max mic distance from centroid, computed in prepare()

        // --------------------------------------------------------------
        // Parameters
        // --------------------------------------------------------------
        std::atomic<bool>  m_enabled              { false };
        std::atomic<int>   m_maxSources           { 3 };
        std::atomic<float> m_confidenceThreshold  { 10.0f };
        std::atomic<int>   m_numBufferedBlocks    { 8 };
        std::atomic<float> m_searchGridResolution { 0.5f };
        std::atomic<int>   m_maxBins              { 8 };
        std::atomic<float> m_bandHzMin            { 400.0f };
        std::atomic<float> m_bandHzMax            { 4000.0f };
        std::atomic<float> m_yRangeMin            { -1.0f };
        std::atomic<float> m_yRangeMax            { 2.0f };

        // --------------------------------------------------------------
        // Rolling analysis window (audio thread writes)
        // --------------------------------------------------------------
        static constexpr int MAX_BLOCK_SIZE_SUPPORTED    = 4096;
        static constexpr int MAX_BUFFERED_BLOCKS_SUPPORTED = 32;
        std::vector<std::vector<float>> m_windowBuffer;   // [channel][sample]
        int m_windowWritePos          = 0;
        int m_windowTargetSamples     = 0;   // maxBlockSize * numBufferedBlocks, latched at window start
        int m_maxBlockSize            = 0;

        // Hand-off to the background thread: when a window fills, its
        // content is copied here and the background thread is signalled.
        // Copy (not pointer swap) because the audio thread must keep
        // writing into m_windowBuffer immediately for the next window.
        std::mutex                      m_pendingWindowMutex;
        std::vector<std::vector<float>> m_pendingWindow;
        bool                             m_pendingWindowReady = false;
        std::condition_variable         m_pendingWindowCv;

        // --------------------------------------------------------------
        // Detected sources (background thread writes, audio thread reads)
        // --------------------------------------------------------------
        mutable std::mutex m_sourcesMutex;
        std::vector<SixDofSourcePosition> m_detectedSources;

        // --------------------------------------------------------------
        // Detection failure protection.
        //
        // If a localization pass finds zero sources above confidenceThreshold
        // (e.g. near-silence, or a genuinely empty analysis window), we do
        // NOT immediately blank the mask — that would click/mute on every
        // brief gap. Instead:
        //   Valid  -> normal operation, m_detectedSources is fresh.
        //   Stale  -> last known-good sources are kept AS-IS for up to
        //             MAX_STALE_CYCLES consecutive empty localization
        //             passes (silently reusing possibly-outdated positions
        //             is safer short-term than an audible mask glitch).
        //   Bypass -> after MAX_STALE_CYCLES, give up on the stale data
        //             (it's more likely wrong than useful by then) and
        //             fall back to full pass-through (gain=1 every
        //             channel) until a fresh detection succeeds again.
        // Background thread owns the transitions; audio thread only reads
        // m_state (relaxed atomic, no lock needed for this coarse flag).
        // --------------------------------------------------------------
        enum class State { Valid, Stale, Bypass };
        std::atomic<State> m_state{ State::Bypass };
        static constexpr int MAX_STALE_CYCLES = 6;
        int m_staleCycleCount = 0;

        // --------------------------------------------------------------
        // Per-channel smoothed mask gain (audio thread only, no allocation
        // after prepare())
        // --------------------------------------------------------------
        std::vector<juce::LinearSmoothedValue<float>> m_channelGainSmoothers;
        static constexpr float GAIN_SMOOTH_TIME_SECONDS = 0.05f;

        // --------------------------------------------------------------
        // FFT (JUCE only — no third-party dependency). Sized for a SINGLE
        // STFT analysis frame (STFT_FRAME_SIZE), reused many times per
        // analysis window (one FFT per hop per channel) — unlike the
        // previous GCC-PHAT implementation, which sized its FFT for the
        // WHOLE window at once (linear cross-correlation).
        // --------------------------------------------------------------
        static constexpr int STFT_FRAME_SIZE = 1024;
        static constexpr int STFT_HOP_SIZE   = 512; // 50% overlap, matches the Python prototypes
        std::unique_ptr<juce::dsp::FFT> m_fft;
        int m_fftOrder = 0;
        // Scratch complex buffer (interleaved re,im), reused across calls
        // on the background thread only — never touched by the audio thread.
        std::vector<float> m_fftScratch;

        // --------------------------------------------------------------
        // Background thread lifecycle
        // --------------------------------------------------------------
        std::thread       m_backgroundThread;
        std::atomic<bool> m_shouldStop{ false };

        static constexpr float SPEED_OF_SOUND  = 340.0f;
        static constexpr float MIN_DIST        = 0.05f;
        static constexpr float MASK_TRANSITION = 0.3f;   // metres, matches Python default
    };
}
