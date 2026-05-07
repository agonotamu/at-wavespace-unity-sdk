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
        const int fftSize = m_table->getFFTSize();
        const int bufSize = 2 * fftSize;   // JUCE FFT uses 2×N for complex pairs

        m_workBufIn .assign(bufSize, 0.0f);
        m_workBufL  .assign(bufSize, 0.0f);
        m_workBufR  .assign(bufSize, 0.0f);
        m_workBufOldL.assign(bufSize, 0.0f);
        m_workBufOldR.assign(bufSize, 0.0f);
        m_overlapL  .assign(fftSize, 0.0f);
        m_overlapR  .assign(fftSize, 0.0f);
    }
}

void HRTFProcessor::reset()
{
    std::fill(m_overlapL.begin(),    m_overlapL.end(),    0.0f);
    std::fill(m_overlapR.begin(),    m_overlapR.end(),    0.0f);
    std::fill(m_workBufIn.begin(),   m_workBufIn.end(),   0.0f);
    std::fill(m_workBufL.begin(),    m_workBufL.end(),    0.0f);
    std::fill(m_workBufR.begin(),    m_workBufR.end(),    0.0f);
    std::fill(m_workBufOldL.begin(), m_workBufOldL.end(), 0.0f);
    std::fill(m_workBufOldR.begin(), m_workBufOldR.end(), 0.0f);
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
    if (m_workBufIn.empty()) return;

    const int   fftSize    = m_table->getFFTSize();
    const int   M          = m_table->getIRLength();
    const auto& fft        = m_table->getFFT();
    const int   overlapLen = M - 1;

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

    const float* hL_lo = m_table->getFFT_L(idxLower);
    const float* hL_hi = m_table->getFFT_L(idxUpper);
    const float* hR_lo = m_table->getFFT_R(idxLower);
    const float* hR_hi = m_table->getFFT_R(idxUpper);
    const float  oneMinusAlpha = 1.0f - alpha;
    const int    bufSize = 2 * fftSize;

    float* hBlendL = m_workBufOldL.data();
    float* hBlendR = m_workBufOldR.data();

    // LERP complex IR FFTs — auto-vectorised to NEON/AVX with -O2
    for (int k = 0; k < bufSize; ++k)
    {
        hBlendL[k] = oneMinusAlpha * hL_lo[k] + alpha * hL_hi[k];
        hBlendR[k] = oneMinusAlpha * hR_lo[k] + alpha * hR_hi[k];
    }

    // ── Step 3 : X × H_blend → IFFT ─────────────────────────────────────────
    std::memcpy(m_workBufL.data(), m_workBufIn.data(), bufSize * sizeof(float));
    complexMultiply(m_workBufL.data(), hBlendL, fftSize);
    fft.performRealOnlyInverseTransform(m_workBufL.data());

    std::memcpy(m_workBufR.data(), m_workBufIn.data(), bufSize * sizeof(float));
    complexMultiply(m_workBufR.data(), hBlendR, fftSize);
    fft.performRealOnlyInverseTransform(m_workBufR.data());

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
