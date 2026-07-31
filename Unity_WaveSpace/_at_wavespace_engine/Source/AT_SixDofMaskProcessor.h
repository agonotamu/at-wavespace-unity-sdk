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
 * Ported from the validated Python prototype (apply_mask.py, FocalSources
 * project):
 *   - Localization: GCC-PHAT (Knapp & Carter 1976) per channel vs a
 *     reference channel, + spherical interpolation (Smith & Abel 1987,
 *     closed-form TDOA multilateration) — no grid search, no
 *     eigendecomposition. DORT/MUSIC were evaluated and ruled out for
 *     real-time cost; see the 6dof-nav skill for the full comparison.
 *   - Multi-source detection: mode search (most frequent position) over a
 *     rolling history of single-window estimates, NOT k-means — validated
 *     as robust to "compromise" positions produced when several sources are
 *     simultaneously active in the same analysis window (those scatter and
 *     never repeat, unlike a genuinely dominant source's estimate).
 *   - Masking: per-channel gain from the "source must lie between listener
 *     and microphone" validity criterion, combined (max) across all
 *     detected sources. No source separation/extraction is performed —
 *     the mask is applied directly to the captured signal already present
 *     in the player's buffer, which is sufficient for navigation (as
 *     opposed to per-source isolation/export, out of scope here).
 *
 * Dependencies: JUCE only (juce::dsp::FFT for GCC-PHAT, std:: for threading
 * and containers). No third-party numerical libraries.
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
     * localization (GCC-PHAT is too expensive to run on the audio thread);
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

        /// Grid resolution (metres) for the mode-detection histogram.
        /// Default 0.02 — validated on the clean synthetic corpus (first
        /// FocalSources test set). Real, reverberant recordings typically
        /// need a coarser value (0.1-0.3) — see 6dof-nav skill.
        void setGridRes(float gridRes);
        float getGridRes() const { return m_gridRes.load(std::memory_order_relaxed); }

        /// Minimum number of matching estimates in the rolling history for a
        /// mode to be accepted as a real source. Default 4 (synthetic corpus).
        void setMinBlockCount(int minBlockCount);
        int getMinBlockCount() const { return m_minBlockCount.load(std::memory_order_relaxed); }

        /// Number of audio callback blocks accumulated into one localization
        /// analysis window (1 = lowest latency; try this first — one block
        /// up to 4096 samples @ 48 kHz is ~85 ms. Increase if GCC-PHAT proves
        /// unreliable on windows that short on real/reverberant material).
        void setNumBufferedBlocks(int numBufferedBlocks);
        int getNumBufferedBlocks() const { return m_numBufferedBlocks.load(std::memory_order_relaxed); }

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

        static constexpr int MAX_SOURCES = 6;

    private:
        // --------------------------------------------------------------
        // Internal: localization (background thread)
        // --------------------------------------------------------------
        void backgroundThreadLoop();
        void runLocalizationOnWindow(const std::vector<std::vector<float>>& window);
        bool gccPhatDelaySamples(const float* ref, const float* ch, int numSamples, double& outDelaySamples);
        bool sphericalInterpolation(const std::vector<double>& delaysSeconds,
                                     const std::vector<char>& validMask,
                                     SixDofSourcePosition& outPos,
                                     double& outResidual) const;
        void pushEstimateAndUpdateModes(const SixDofSourcePosition& estimate);

        /**
         * @brief One candidate source tracked ACROSS mode-detection cycles,
         * for the per-source temporal hysteresis (see pushEstimateAndUpdateModes).
         *
         * Distinct from the global Valid/Stale/Bypass state machine: that one
         * handles TOTAL detection failure (zero sources for many cycles this
         * one smooths a single noisy cycle for an INDIVIDUAL source (e.g. its
         * mode count grazing min_block_count and dipping below it for one
         * cycle) so it doesn't vanish from/reappear in m_detectedSources
         * instantly — diagnosed via console logs as the cause of a brief,
         * audible "dropout": source count sequence ...2,2,2,1,2,2,2... for a
         * single missed cycle.
         */
        struct TrackedCandidate
        {
            SixDofSourcePosition pos;
            int  presentStreak = 0;   ///< consecutive cycles seen (for promotion to confirmed)
            int  absentStreak  = 0;   ///< consecutive cycles missing (for demotion/removal)
            bool confirmed     = false;
        };
        std::vector<TrackedCandidate> m_trackedCandidates;

        /// A brand-new candidate must be seen this many consecutive cycles
        /// before being exposed via m_detectedSources — filters one-off
        /// phantom "compromise" positions that occasionally slip past the
        /// fit-residual filter (observed: a spurious 3rd source appearing
        /// for a few cycles near the end of a real test log).
        static constexpr int SOURCE_CONFIRM_CYCLES = 2;

        /// An already-confirmed source can be missing this many consecutive
        /// cycles before being dropped — the actual fix for the single-cycle
        /// dropout described above.
        static constexpr int SOURCE_GRACE_CYCLES = 2;

        /// Distance threshold for matching a new-cycle estimate to an
        /// existing tracked candidate (metres). Same scale as the mode
        /// bucket separation used historically in the Python prototype.
        static constexpr float SOURCE_MATCH_DIST = 0.5f;

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
        double m_maxDelaySeconds   = 0.05;     // 2x array extent / c, margin, computed in prepare()

        // --------------------------------------------------------------
        // Parameters
        // --------------------------------------------------------------
        std::atomic<bool>  m_enabled          { false };
        std::atomic<float> m_gridRes          { 0.02f };
        std::atomic<int>   m_minBlockCount    { 4 };
        std::atomic<int>   m_numBufferedBlocks{ 1 };

        // --------------------------------------------------------------
        // Rolling analysis window (audio thread writes)
        // --------------------------------------------------------------
        static constexpr int MAX_BLOCK_SIZE_SUPPORTED    = 4096;
        static constexpr int MAX_BUFFERED_BLOCKS_SUPPORTED = 16;
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
        // Rolling history of single-window position estimates (background
        // thread only) — mode detection runs over this, adapting the
        // Python prototype's "per-block estimates list" to streaming.
        // --------------------------------------------------------------
        static constexpr int HISTORY_SIZE = 32;
        std::vector<SixDofSourcePosition> m_estimateHistory;
        int m_historyWritePos = 0;
        int m_historyCount    = 0;

        // --------------------------------------------------------------
        // Detected sources (background thread writes, audio thread reads)
        // --------------------------------------------------------------
        mutable std::mutex m_sourcesMutex;
        std::vector<SixDofSourcePosition> m_detectedSources;

        // --------------------------------------------------------------
        // Detection failure protection.
        //
        // If a localization pass finds zero sources above min_block_count
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
        // FFT (JUCE only — no third-party dependency)
        // --------------------------------------------------------------
        std::unique_ptr<juce::dsp::FFT> m_fft;
        int m_fftOrder = 0;
        int m_fftSize  = 0;
        // Scratch complex buffers (interleaved re,im), reused across calls
        // on the background thread only — never touched by the audio thread.
        std::vector<float> m_fftBufA, m_fftBufB;

        // --------------------------------------------------------------
        // Background thread lifecycle
        // --------------------------------------------------------------
        std::thread       m_backgroundThread;
        std::atomic<bool> m_shouldStop{ false };

        static constexpr float SPEED_OF_SOUND  = 340.0f;
        static constexpr float MIN_DIST        = 0.05f;
        static constexpr float MASK_TRANSITION = 0.3f;   // metres, matches Python default

        // Fit-quality rejection threshold for sphericalInterpolation()'s
        // residual (see its implementation comment). Clean single-source
        // data: ~0 (validated ~1e-10). Data mixed from two simultaneously
        // active sources: ~4+ (validated, ~7 orders of magnitude higher) —
        // this threshold has enormous margin either way, not a fine-tuned
        // knob. Units: same as the TDOA equations' RHS (~metres^2 scale).
        static constexpr double MAX_FIT_RESIDUAL = 0.5;
        static constexpr int   REF_CHANNEL     = 0;
    };
}
