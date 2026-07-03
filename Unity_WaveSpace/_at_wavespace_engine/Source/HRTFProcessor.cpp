#include "HRTFProcessor.h"
#include <cstring>
#include <algorithm>

// ────────────────────────────────────────────────────────────────────────────
// LIFECYCLE
// ────────────────────────────────────────────────────────────────────────────

HRTFProcessor::HRTFProcessor() = default;

void HRTFProcessor::prepare(double sampleRate, int samplesPerBlock)
{
    m_sampleRate   = sampleRate;
    m_maxBlockSize = samplesPerBlock;

    if (m_table && m_table->isFFTPrepared())
    {
        const int fftOrder = m_table->getFFTOrder();
        const int fftSize  = m_table->getFFTSize();   // == 1 << fftOrder
        const int bufSize  = 2 * fftSize;

        // Block until olaBlock finishes its current block (at most one callback
        // duration, typically < 43 ms).  This prevents reallocating work buffers
        // while the audio thread holds raw pointers into them.
        std::lock_guard<std::mutex> lock(m_bufferMutex);

        // Create a private FFT instance with the same order as the table.
        // Owning a separate instance ensures that a concurrent prepareFFT()
        // call (which rebuilds the table's internal FFT) never invalidates
        // twiddle factors that are in use by this processor.
        m_fft = std::make_unique<juce::dsp::FFT>(fftOrder);

        m_workBufIn .assign(bufSize, 0.0f);
        m_workBufL  .assign(bufSize, 0.0f);
        m_workBufR  .assign(bufSize, 0.0f);
        m_workBufOldL.assign(bufSize, 0.0f);
        m_workBufOldR.assign(bufSize, 0.0f);
        m_overlapL  .assign(fftSize, 0.0f);
        m_overlapR  .assign(fftSize, 0.0f);

        // Dual-convolution crossfade state (see header): previous-filter
        // storage + scratch for the old-filter convolution.
        m_prevHBlendL .assign(bufSize, 0.0f);
        m_prevHBlendR .assign(bufSize, 0.0f);
        m_workBufPrevL.assign(bufSize, 0.0f);
        m_workBufPrevR.assign(bufSize, 0.0f);
        m_prevHBlendValid = false;
        m_prevIdxLower = m_prevIdxUpper = -1;
        m_prevAlpha = -1.0f;
    }
}

// ────────────────────────────────────────────────────────────────────────────
// clearOverlapTail — lightweight, non-blocking reset of the OLA overlap
// buffers only (m_overlapL/R). Used when a channel resumes processing after
// being skipped for several blocks by the amplitude-based HRTF bypass in
// SpatializationEngine::processBinauralVirtualization(). While skipped,
// olaBlock() is never called, so the overlap tail is left stale: it still
// holds the IR tail from whatever azimuth was current when the channel went
// silent. Re-adding that stale tail on resume produces an audible click,
// since it gets mixed with the new block computed for the current (possibly
// very different) azimuth. Clearing it here removes that artefact.
//
// Deliberately NOT reusing reset(): reset() touches m_workBufIn/L/R as well,
// which prepare() may be concurrently resizing under m_bufferMutex during an
// HRTF reload — calling the full reset() from the audio thread at the wrong
// moment risks racing that resize. This function only touches the overlap
// buffers and uses the same non-blocking lock pattern as olaBlock(), so it
// is safe to call from the audio thread and simply no-ops (silently) if a
// reload is in progress — in that case the overlap buffers are about to be
// reallocated by prepare() anyway, so skipping is harmless.
void HRTFProcessor::clearOverlapTail() noexcept
{
    std::unique_lock<std::mutex> lock(m_bufferMutex, std::try_to_lock);
    if (!lock) return;

    std::fill(m_overlapL.begin(), m_overlapL.end(), 0.0f);
    std::fill(m_overlapR.begin(), m_overlapR.end(), 0.0f);

    // Also invalidate the stored previous filter so the next block runs a
    // single convolution with a fresh snap (no crossfade from a stale filter) —
    // same rationale as clearing the OLA tail itself.
    m_prevHBlendValid = false;
    m_prevIdxLower = m_prevIdxUpper = -1;
    m_prevAlpha = -1.0f;
}

void HRTFProcessor::reset()
{
    std::fill(m_overlapL.begin(),        m_overlapL.end(),        0.0f);
    std::fill(m_overlapR.begin(),        m_overlapR.end(),        0.0f);
    std::fill(m_workBufIn.begin(),       m_workBufIn.end(),       0.0f);
    std::fill(m_workBufL.begin(),        m_workBufL.end(),        0.0f);
    std::fill(m_workBufR.begin(),        m_workBufR.end(),        0.0f);
    std::fill(m_workBufOldL.begin(),     m_workBufOldL.end(),     0.0f);
    std::fill(m_workBufOldR.begin(),     m_workBufOldR.end(),     0.0f);
    m_prevHBlendValid = false;
    m_prevIdxLower = m_prevIdxUpper = -1;
    m_prevAlpha = -1.0f;
}

void HRTFProcessor::setHRTFTable(const HRTFTable* table)
{
    m_table        = table;
    m_currentIndex = 0;

    if (m_maxBlockSize > 0 && table && table->isFFTPrepared())
        prepare(m_sampleRate, m_maxBlockSize);
    else
        reset();
}

void HRTFProcessor::preloadIR(float azimuth, float elevation)
{
    if (!m_table || !m_table->isFFTPrepared()) return;
    m_currentIndex = m_table->getNearestIndex(azimuth, elevation);
}

// ────────────────────────────────────────────────────────────────────────────
// CORE OLA
// ────────────────────────────────────────────────────────────────────────────

void HRTFProcessor::complexMultiply(float* io, const float* h, int fftSize) noexcept
{
    // io and h: 2*fftSize floats, JUCE interleaved format [re0,im0, re1,im1, …]
    // Compiler will auto-vectorise (NEON/AVX) with -O2.
    for (int k = 0; k < fftSize; ++k)
    {
        const float xRe = io[2 * k];
        const float xIm = io[2 * k + 1];
        const float hRe = h [2 * k];
        const float hIm = h [2 * k + 1];
        io[2 * k]     = xRe * hRe - xIm * hIm;
        io[2 * k + 1] = xRe * hIm + xIm * hRe;
    }
}

void HRTFProcessor::olaBlock(float*       outL,
                               float*       outR,
                               const float* src,
                               int          numSamples,
                               float        azimuth,
                               float        elevation,
                               bool         accumulate)
{
    if (!m_table || !m_table->isFFTPrepared()) return;
    if (!m_fft || m_workBufIn.empty()) return;

    // Non-blocking try_lock: if prepare() is currently reallocating buffers
    // on another thread, skip this block rather than blocking the RT audio thread.
    // prepare() holds the lock for at most the allocation time (~microseconds),
    // so a skipped block here is rare and produces at most a brief silent frame.
    std::unique_lock<std::mutex> lock(m_bufferMutex, std::try_to_lock);
    if (!lock) return;

    const int fftSize    = m_fft->getSize();   // authoritative: from our own FFT instance
    const int M          = m_table->getIRLength();
    const int overlapLen = M - 1;

    // Guard: if the table was rebuilt with a different block size after our
    // last prepare() call, the buffer sizes would mismatch the FFT size.
    // Skip this block; the next prepare() call will realign everything.
    if ((int)m_workBufIn.size() != 2 * fftSize ||
        (int)m_workBufL.size()  != 2 * fftSize ||
        (int)m_workBufR.size()  != 2 * fftSize)
        return;

    // Bind to this processor's OWN FFT object — never the table's.
    // If prepareFFT() is called concurrently (HRTF reload), the table's FFT
    // is destroyed and recreated; using m_table->getFFT() here would give a
    // dangling reference and crash in butterfly4 / operator*=.
    const juce::dsp::FFT& fft = *m_fft;

    // ── Step 1 : forward FFT of zero-padded input ────────────────────────────
    std::fill(m_workBufIn.begin(), m_workBufIn.end(), 0.0f);
    std::memcpy(m_workBufIn.data(), src, static_cast<size_t>(numSamples) * sizeof(float));
    fft.performRealOnlyForwardTransform(m_workBufIn.data());

    // ── Step 2 : frequency-domain HRTF interpolation ─────────────────────────
    //
    // Find the two SOFA bins bracketing the current azimuth and blend their FFTs:
    //   H_blend[k] = (1 - α) × H_lower[k] + α × H_upper[k]
    //
    // The filter changes continuously every block → no discrete bin switch → no clicks.
    // m_workBufOldL/R are scratch buffers for H_blend (pre-allocated in prepare()).

    int   idxLower, idxUpper;
    float alpha;
    m_table->findBracketingIndices(azimuth, elevation, idxLower, idxUpper, alpha);


    float* hBlendL = m_workBufOldL.data();
    float* hBlendR = m_workBufOldR.data();
    const int bufSize = 2 * fftSize;

    // Blend the two bracketing HRTF bins under a shared lock on m_fftData.
    // tryBlendHRTF() returns false if prepareFFT() is concurrently reallocating
    // m_fftData (exclusive lock held).  In that case we output silence for this
    // block rather than reading a dangling pointer — which was the crash cause.
    if (!m_table->tryBlendHRTF(idxLower, idxUpper, alpha,
                                 hBlendL, hBlendR, bufSize))
        return;

    // ── Step 3 : convolution(s) + crossfade ──────────────────────────────────
    //
    // The HRTF filter is inherently block-rate: one H_blend per olaBlock()
    // call, held constant across the block. During continuous motion this
    // steps the filter at the block rate (≈23.4 Hz @ 2048/48k) — audible as
    // periodic modulation/clicks in rotation AND translation, since both
    // change the virtual-speaker azimuths. (A frequency-domain EMA was tried
    // here before: it shrinks the step size but keeps the step cadence.)
    //
    // Fix: when the filter changed since the previous block, convolve with
    // BOTH filters and crossfade linearly across the block:
    //
    //   y[n] = (1 − r[n]) · (x ∗ h_prev)[n]  +  r[n] · (x ∗ h_new)[n],
    //   r[n] = (n + 1) / numSamples          (reaches exactly 1 at block end)
    //
    // The effective filter trajectory becomes piecewise-linear in time —
    // continuous at every sample, no step at block boundaries. The overlap
    // tail saved for the next block is the NEW filter's tail (r = 1 at the
    // boundary, so the new filter is what carries forward); the discarded
    // part of the old filter's tail is the same second-order residue any
    // fading convolver accepts, inaudible next to the step it removes.
    //
    // When the filter is unchanged (static listener/speakers — bracket pair
    // AND alpha identical), the single-convolution fast path below is
    // bit-identical to the original code, so the extra cost only applies
    // while actually moving.
    const bool filterChanged = m_prevHBlendValid
                               && (idxLower != m_prevIdxLower
                                   || idxUpper != m_prevIdxUpper
                                   || alpha    != m_prevAlpha);

    // New-filter convolution → m_workBufL/R (always needed).
    std::memcpy(m_workBufL.data(), m_workBufIn.data(), bufSize * sizeof(float));
    complexMultiply(m_workBufL.data(), hBlendL, fftSize);
    fft.performRealOnlyInverseTransform(m_workBufL.data());

    std::memcpy(m_workBufR.data(), m_workBufIn.data(), bufSize * sizeof(float));
    complexMultiply(m_workBufR.data(), hBlendR, fftSize);
    fft.performRealOnlyInverseTransform(m_workBufR.data());

    if (filterChanged)
    {
        // Old-filter convolution → m_workBufPrevL/R.
        std::memcpy(m_workBufPrevL.data(), m_workBufIn.data(), bufSize * sizeof(float));
        complexMultiply(m_workBufPrevL.data(), m_prevHBlendL.data(), fftSize);
        fft.performRealOnlyInverseTransform(m_workBufPrevL.data());

        std::memcpy(m_workBufPrevR.data(), m_workBufIn.data(), bufSize * sizeof(float));
        complexMultiply(m_workBufPrevR.data(), m_prevHBlendR.data(), fftSize);
        fft.performRealOnlyInverseTransform(m_workBufPrevR.data());

        // Crossfade old → new in place into m_workBufL/R over the audible part
        // of the block, so Steps 4/5 below need no special casing. The tail
        // region [numSamples, fftSize) keeps the pure new-filter result.
        const float invN = 1.0f / static_cast<float>(numSamples);
        for (int n = 0; n < numSamples; ++n)
        {
            const float r = static_cast<float>(n + 1) * invN;
            const float s = 1.0f - r;
            m_workBufL[n] = s * m_workBufPrevL[n] + r * m_workBufL[n];
            m_workBufR[n] = s * m_workBufPrevR[n] + r * m_workBufR[n];
        }
    }

    // Store this block's filter as "previous" for the next block.
    std::memcpy(m_prevHBlendL.data(), hBlendL, static_cast<size_t>(bufSize) * sizeof(float));
    std::memcpy(m_prevHBlendR.data(), hBlendR, static_cast<size_t>(bufSize) * sizeof(float));
    m_prevHBlendValid = true;
    m_prevIdxLower = idxLower;
    m_prevIdxUpper = idxUpper;
    m_prevAlpha    = alpha;

    // ── Step 4 : overlap-add output ──────────────────────────────────────────
    if (accumulate)
    {
        for (int n = 0; n < numSamples; ++n)
        {
            outL[n] += m_workBufL[n] + m_overlapL[n];
            outR[n] += m_workBufR[n] + m_overlapR[n];
        }
    }
    else
    {
        for (int n = 0; n < numSamples; ++n)
        {
            outL[n] = m_workBufL[n] + m_overlapL[n];
            outR[n] = m_workBufR[n] + m_overlapR[n];
        }
    }

    // ── Step 5 : save overlap tail for next block ─────────────────────────────
    if (overlapLen > 0)
    {
        std::memcpy(m_overlapL.data(), m_workBufL.data() + numSamples,
                    static_cast<size_t>(overlapLen) * sizeof(float));
        std::memcpy(m_overlapR.data(), m_workBufR.data() + numSamples,
                    static_cast<size_t>(overlapLen) * sizeof(float));
    }
}

void HRTFProcessor::processAndAccumulate(juce::AudioBuffer<float>& outputBuffer,
                                          const juce::AudioBuffer<float>& sourceBuffer,
                                          int sourceChannel, int numSamples,
                                          float azimuth, float elevation)
{
    if (!m_table || !m_table->isFFTPrepared()) return;
    if (sourceChannel >= sourceBuffer.getNumChannels()) return;
    if (outputBuffer.getNumChannels() < 2) return;
    if (numSamples <= 0) return;

    // Update nearest index for debugging / preloadIR coherence
    m_currentIndex = m_table->getNearestIndex(azimuth, elevation);

    olaBlock(outputBuffer.getWritePointer(0),
             outputBuffer.getWritePointer(1),
             sourceBuffer.getReadPointer(sourceChannel),
             numSamples,
             azimuth, elevation,
             /*accumulate=*/true);
}

void HRTFProcessor::processAndAccumulate(juce::AudioBuffer<float>& outputBuffer,
                                          const juce::AudioBuffer<float>& sourceBuffer,
                                          int sourceChannel, int numSamples,
                                          float azimuth, float elevation,
                                          float /*distance*/)
{
    processAndAccumulate(outputBuffer, sourceBuffer,
                         sourceChannel, numSamples, azimuth, elevation);
}

// ────────────────────────────────────────────────────────────────────────────
// process  (simple binaural path — in-place)
// ────────────────────────────────────────────────────────────────────────────

void HRTFProcessor::process(juce::AudioBuffer<float>& buffer,
                              float azimuth, float elevation)
{
    const int numSamples = buffer.getNumSamples();
    if (!m_table || !m_table->isFFTPrepared() || numSamples <= 0) return;
    if (buffer.getNumChannels() < 1) return;

    m_azimuth.store(azimuth);
    m_elevation.store(elevation);
    m_currentIndex = m_table->getNearestIndex(azimuth, elevation);

    // Channel 0 is both source and dest → use a local copy for the input.
    // m_workBufIn is large enough (allocated in prepare).
    const float* src = buffer.getReadPointer(0);

    // Temporary: borrow m_workBufIn storage for the copy
    // (it will be overwritten in olaBlock anyway)
    std::vector<float> srcCopy(src, src + numSamples);

    // Zero output before accumulation
    if (buffer.getNumChannels() >= 2)
        buffer.clear();

    olaBlock(buffer.getWritePointer(0),
             buffer.getNumChannels() >= 2 ? buffer.getWritePointer(1)
                                          : buffer.getWritePointer(0),
             srcCopy.data(),
             numSamples,
             azimuth, elevation,
             /*accumulate=*/false);
}

void HRTFProcessor::process(juce::AudioBuffer<float>& buffer,
                              float azimuth, float elevation, float distance)
{
    m_distance.store(distance);
    process(buffer, azimuth, elevation);
}
