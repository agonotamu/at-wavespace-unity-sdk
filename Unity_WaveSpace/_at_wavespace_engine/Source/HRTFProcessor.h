#pragma once

#include <JuceHeader.h>
#include "HRTFTable.h"
#include <vector>
#include <atomic>
#include <mutex>

/**
 * @file HRTFProcessor.h
 * @brief Binaural processor — frequency-domain OLA convolution via shared HRTFTable.
 * @author Antoine Gonot  @date 2026
 *
 * Per-instance state (all pre-allocated in prepare()):
 *   m_workBufIn  — 2*fftSize floats — FFT of the current input block
 *   m_workBufL/R — 2*fftSize floats — FFT output × H_L/R, then IFFT result
 *   m_overlapL/R — fftSize floats   — OLA tail from previous block
 *
 * OLA per block (inside processAndAccumulate):
 *   1. Zero-pad input[0..N-1] into m_workBufIn[0..2F-1]
 *   2. m_fft.performRealOnlyForwardTransform(m_workBufIn)   — in-place
 *   3. m_workBufL ← m_workBufIn × H_L[currentIndex]        — complex multiply
 *      m_workBufR ← m_workBufIn × H_R[currentIndex]
 *   4. m_fft.performRealOnlyInverseTransform(m_workBufL/R)  — in-place
 *      → m_workBufL/R[0..fftSize-1] are the real output samples
 *   5. outL[n] += m_workBufL[n] + m_overlapL[n]  for n in [0, N-1]
 *   6. m_overlapL[0..M-2] ← m_workBufL[N..N+M-2] (tail for next block)
 *
 * IR switch = m_currentIndex = newIndex   — O(1), no FFT, no heap alloc.
 */
class HRTFProcessor
{
public:
    HRTFProcessor();
    ~HRTFProcessor() = default;

    HRTFProcessor(const HRTFProcessor&)            = delete;
    HRTFProcessor& operator=(const HRTFProcessor&) = delete;

    // ── Setup ─────────────────────────────────────────────────────────────────

    /**
     * @brief Allocates per-instance OLA buffers.
     * Must be called after HRTFTable::prepareFFT() so that getFFTSize() is valid.
     */
    void prepare(double sampleRate, int samplesPerBlock);

    /** Zeros overlap buffers (call on transport stop/rewind). */
    void reset();

    /**
     * @brief Lightweight, non-blocking reset of the OLA overlap tail only
     * (m_overlapL/R), leaving m_workBufIn/L/R untouched.
     *
     * Call this when a channel resumes HRTF processing after having been
     * skipped for several blocks by an amplitude-based bypass (e.g.
     * SpatializationEngine's HRTF_SKIP_THRESHOLD / HRTF_HOLD_BLOCKS logic).
     * While skipped, olaBlock() never runs, so the overlap tail is left
     * stale — it still holds the IR tail computed for whatever azimuth was
     * current when the channel went silent. Re-adding that stale tail on
     * resume mixes it with the new block computed for the current (possibly
     * very different) azimuth, producing an audible click. This clears it.
     *
     * Safe to call from the audio thread: uses the same non-blocking
     * try_lock pattern as olaBlock() on m_bufferMutex, and silently no-ops
     * if prepare() currently holds the lock (in which case the overlap
     * buffers are about to be reallocated anyway).
     *
     * Deliberately distinct from reset(): reset() also touches
     * m_workBufIn/L/R, which prepare() may be concurrently resizing — not
     * safe to call unconditionally from the audio thread.
     */
    void clearOverlapTail() noexcept;

    // ── HRTF table ────────────────────────────────────────────────────────────

    /**
     * @brief Attaches this processor to the shared HRTFTable.
     * The table must have been prepared with prepareFFT() before any audio
     * processing starts.  Resets overlap buffers.
     */
    void setHRTFTable(const HRTFTable* table);

    bool isHRTFLoaded() const { return m_table != nullptr && m_table->isFFTPrepared(); }

    // ── Index control ─────────────────────────────────────────────────────────

    void setCurrentIndex(int idx) { m_currentIndex = idx; }
    int  getCurrentIndex()  const { return m_currentIndex; }

    /** Pre-warms current index before fade-in (replaces old preloadIR). */
    void preloadIR(float azimuth, float elevation = 0.0f);

    // ── Position setters (simple-binaural path) ───────────────────────────────

    void setAzimuth  (float az)   { m_azimuth.store(az);   }
    void setElevation(float el)   { m_elevation.store(el); }
    void setDistance (float dist) { m_distance.store(dist);}

    float getAzimuth()   const { return m_azimuth.load();   }
    float getElevation() const { return m_elevation.load(); }
    float getDistance()  const { return m_distance.load();  }

    bool  hasDistanceInfo() const { return false; }
    float getMinDistance()  const { return 1.0f;  }
    float getMaxDistance()  const { return 1.0f;  }

    // ── Processing ────────────────────────────────────────────────────────────

    /**
     * @brief [WFS binaural virtualisation path]
     * OLA-convolves sourceBuffer[sourceChannel] with the HRTF nearest to
     * (azimuth, elevation) and ACCUMULATES into outputBuffer channels 0 (L)
     * and 1 (R).  IR switch is O(1).
     */
    void processAndAccumulate(juce::AudioBuffer<float>& outputBuffer,
                              const juce::AudioBuffer<float>& sourceBuffer,
                              int sourceChannel, int numSamples,
                              float azimuth, float elevation);

    /** @brief Distance overload — delegates to azimuth-only lookup. */
    void processAndAccumulate(juce::AudioBuffer<float>& outputBuffer,
                              const juce::AudioBuffer<float>& sourceBuffer,
                              int sourceChannel, int numSamples,
                              float azimuth, float elevation, float distance);

    /**
     * @brief [Simple binaural path]
     * In-place OLA spatialisation. Channel 0 = mono input; writes L→ch0, R→ch1.
     */
    void process(juce::AudioBuffer<float>& buffer, float azimuth, float elevation);
    void process(juce::AudioBuffer<float>& buffer,
                 float azimuth, float elevation, float distance);

private:

    const HRTFTable* m_table = nullptr;
    int              m_currentIndex = 0;   ///< nearest SOFA bin (for preloadIR / debug)

    // Per-instance OLA buffers (allocated in prepare(), never in audio thread)
    std::vector<float> m_workBufIn;    ///< 2 * fftSize  — input FFT (shared L/R)
    std::vector<float> m_workBufL;     ///< 2 * fftSize  — convolution result, left
    std::vector<float> m_workBufR;     ///< 2 * fftSize  — convolution result, right
    std::vector<float> m_workBufOldL;  ///< 2 * fftSize  — H_blend scratch, left
    std::vector<float> m_workBufOldR;  ///< 2 * fftSize  — H_blend scratch, right

    // ── Dual-convolution crossfade (block-rate filter-step fix) ──────────────
    //
    // Every per-sample quantity in the engine is smoothed (positions, masks,
    // WFS gains, azimuths, distance gain, fades) — but the HRTF filter itself
    // is inherently BLOCK-rate: one H_blend per olaBlock() call, held constant
    // for the whole block. During continuous listener motion this makes the
    // filter advance in steps at the block rate (2048/48k ≈ 23.4 Hz), audible
    // as periodic clicks/modulation in both rotation AND translation (both
    // change the virtual-speaker azimuths). An EMA on H_blend (tried earlier)
    // reduces the step size but keeps the step CADENCE — the modulation stays.
    //
    // Canonical fix: convolve the block with BOTH the previous block's filter
    // and the new one, and crossfade linearly between the two outputs across
    // the block. The effective filter trajectory becomes piecewise-linear in
    // time — continuous at every sample, with no step at block boundaries.
    // Cost: one extra pair of complex-multiply + IFFT per block per channel,
    // only on blocks where the filter actually changed (skipped when static).
    std::vector<float> m_prevHBlendL;   ///< 2 * fftSize — previous block's filter, L
    std::vector<float> m_prevHBlendR;   ///< 2 * fftSize — previous block's filter, R
    std::vector<float> m_workBufPrevL;  ///< 2 * fftSize — old-filter convolution scratch, L
    std::vector<float> m_workBufPrevR;  ///< 2 * fftSize — old-filter convolution scratch, R
    bool  m_prevHBlendValid = false;    ///< false → first block: single convolution, no fade
    int   m_prevIdxLower = -1;          ///< bracket/alpha of the stored previous filter —
    int   m_prevIdxUpper = -1;          ///<   compared against the current ones to skip
    float m_prevAlpha    = -1.0f;       ///<   the dual path when the filter is unchanged
    std::vector<float> m_overlapL;     ///< fftSize       — OLA tail, left
    std::vector<float> m_overlapR;     ///< fftSize       — OLA tail, right

    // Private FFT instance — owns its own twiddle factors so that a concurrent
    // HRTFTable::prepareFFT() call (which destroys and recreates m_table's FFT)
    // never invalidates an in-progress transform on this processor.
    std::unique_ptr<juce::dsp::FFT> m_fft;

    // Mutex protecting all work buffers and m_fft against concurrent prepare() calls.
    // olaBlock uses try_lock (non-blocking on the RT audio thread).
    // prepare() uses a blocking lock (waits at most one audio block ~42 ms).
    std::mutex m_bufferMutex;

    double m_sampleRate   = 44100.0;
    int    m_maxBlockSize = 512;

    std::atomic<float> m_azimuth   {0.0f};
    std::atomic<float> m_elevation {0.0f};
    std::atomic<float> m_distance  {1.0f};

    // ── Core OLA helpers ──────────────────────────────────────────────────────

    /**
     * @brief One OLA block with frequency-domain HRTF interpolation.
     *
     * Finds the two SOFA bins bracketing the given azimuth, linearly interpolates
     * their FFTs in the frequency domain (H_blend = (1-α)×H_lower + α×H_upper),
     * then does a single complex multiply + IFFT + overlap-add.
     *
     * The HRTF filter changes continuously every block as the azimuth changes.
     * No discrete bin switching → no clicks on fast listener/speaker movement.
     *
     * @param azimuth    Speaker azimuth relative to listener (degrees).
     * @param elevation  Elevation (0 for horizontal WFS arrays).
     * @param accumulate true → outL[n] += result   (processAndAccumulate path)
     *                   false → outL[n]  = result   (process path)
     */
    void olaBlock(float* outL, float* outR,
                  const float* src, int numSamples,
                  float azimuth, float elevation,
                  bool accumulate);

    /**
     * @brief In-place complex multiply: io[k] *= h[k] for k = 0..fftSize-1.
     * Both buffers have 2*fftSize floats in JUCE interleaved format.
     * Auto-vectorised to NEON/AVX by the compiler.
     */
    static void complexMultiply(float* io, const float* h, int fftSize) noexcept;
};
