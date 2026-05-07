#include "HRTFTable.h"
#include <algorithm>
#include <cstring>
#include <cmath>

// ── Stage 1 : build ──────────────────────────────────────────────────────────

bool HRTFTable::build(const SOFAReader& reader, double audioSampleRate, int maxIRLength)
{
    m_built       = false;
    m_fftPrepared = false;

    if (!reader.isLoaded()) return false;

    m_numMeasurements = reader.getNumMeasurements();
    if (m_numMeasurements == 0) return false;

    const int    srcLen = reader.getIRLength();
    const double srcSR  = static_cast<double>(reader.getSampleRate());
    const bool   needsResampling = std::abs(srcSR - audioSampleRate) > 0.1;

    int effectiveLen = srcLen;
    if (needsResampling)
        effectiveLen = static_cast<int>(std::ceil(srcLen * audioSampleRate / srcSR));

    const int limit = (maxIRLength > 0) ? maxIRLength : MAX_IR_LENGTH;
    effectiveLen = std::min(effectiveLen, limit);
    effectiveLen = std::min(effectiveLen, MAX_IR_LENGTH);
    if (effectiveLen <= 0) return false;

    m_irLength = effectiveLen;

    // Flat raw IR storage — ORIGINAL order (not reversed).
    // Layout: [meas_i_L[0..M-1] | meas_i_R[0..M-1]] for each i.
    m_data.assign(static_cast<size_t>(m_numMeasurements) * 2 * m_irLength, 0.0f);

    for (int i = 0; i < m_numMeasurements; ++i)
    {
        std::vector<float> leftIR, rightIR;
        reader.getIRsAtIndex(i, leftIR, rightIR);

        if (needsResampling)
        {
            const double ratio = audioSampleRate / srcSR;
            const int newLen   = static_cast<int>(std::ceil(srcLen * ratio));
            std::vector<float> rL(newLen), rR(newLen);
            resampleIR(leftIR,  rL, ratio);
            resampleIR(rightIR, rR, ratio);
            leftIR  = std::move(rL);
            rightIR = std::move(rR);
        }

        float* dstL = m_data.data() + static_cast<size_t>(i) * 2 * m_irLength;
        float* dstR = dstL + m_irLength;

        // Copy up to m_irLength samples (zero-pad if IR is shorter)
        for (int k = 0; k < m_irLength; ++k)
        {
            dstL[k] = (k < static_cast<int>(leftIR.size()))  ? leftIR[k]  : 0.0f;
            dstR[k] = (k < static_cast<int>(rightIR.size())) ? rightIR[k] : 0.0f;
        }
    }

    m_sofaReader = std::make_shared<SOFAReader>(reader);
    m_built = true;
    return true;
}

// ── Stage 2 : prepareFFT ─────────────────────────────────────────────────────

void HRTFTable::prepareFFT(int blockSize)
{
    if (!m_built || blockSize <= 0) return;
    if (m_fftPrepared && blockSize == m_fftBlockSize) return;  // already done

    // FFT size = smallest power of 2 >= blockSize + irLength - 1
    // This guarantees linear (non-circular) convolution.
    const int minSize = blockSize + m_irLength - 1;
    int order = 0;
    while ((1 << order) < minSize) ++order;

    m_fftSize      = 1 << order;
    m_fftOrder     = order;
    m_fftBlockSize = blockSize;

    // (Re)create the FFT engine for the new size
    m_fft = std::make_unique<juce::dsp::FFT>(order);

    // Allocate FFT data table:
    // [meas0_L[2*fftSize] | meas0_R[2*fftSize] | meas1_L | meas1_R | …]
    // 2*fftSize because JUCE's FFT stores N complex pairs (re/im interleaved)
    const size_t wordsPerMeas = static_cast<size_t>(2) * 2 * m_fftSize;
    m_fftData.assign(static_cast<size_t>(m_numMeasurements) * wordsPerMeas, 0.0f);

    // Temporary work buffer for FFT computation (2*fftSize floats)
    std::vector<float> work(static_cast<size_t>(2) * m_fftSize, 0.0f);

    for (int i = 0; i < m_numMeasurements; ++i)
    {
        const float* srcL = m_data.data() + static_cast<size_t>(i) * 2 * m_irLength;
        const float* srcR = srcL + m_irLength;
        float* dstL = m_fftData.data() + static_cast<size_t>(i) * wordsPerMeas;
        float* dstR = dstL + 2 * m_fftSize;

        // Left IR → FFT
        // juce::dsp::FFT::performRealOnlyForwardTransform:
        //   - input:  first fftSize floats = real signal, rest = 0 (scratch)
        //   - output: 2*fftSize floats = complex spectrum [re0,im0, re1,im1, …]
        std::fill(work.begin(), work.end(), 0.0f);
        std::memcpy(work.data(), srcL, static_cast<size_t>(m_irLength) * sizeof(float));
        m_fft->performRealOnlyForwardTransform(work.data());
        std::memcpy(dstL, work.data(), static_cast<size_t>(2) * m_fftSize * sizeof(float));

        // Right IR → FFT
        std::fill(work.begin(), work.end(), 0.0f);
        std::memcpy(work.data(), srcR, static_cast<size_t>(m_irLength) * sizeof(float));
        m_fft->performRealOnlyForwardTransform(work.data());
        std::memcpy(dstR, work.data(), static_cast<size_t>(2) * m_fftSize * sizeof(float));
    }

    m_fftPrepared = true;
}

// ── HRTF interpolation helpers ───────────────────────────────────────────────

void HRTFTable::findBracketingIndices(float azimuthDeg, float elevationDeg,
                                       int& idxLower, int& idxUpper, float& alpha) const
{
    // Normalize query azimuth to [0, 360)
    float az = azimuthDeg;
    while (az <    0.f) az += 360.f;
    while (az >= 360.f) az -= 360.f;

    // Nearest measurement
    const int nearest   = m_sofaReader->getNearestPositionIndex(az, elevationDeg);
    const float az_near = m_sofaReader->getPositionAzimuth(nearest);

    // Signed angular difference: az - az_near, wrapped to (-180, 180]
    float diff = az - az_near;
    while (diff >  180.f) diff -= 360.f;
    while (diff <= -180.f) diff += 360.f;

    if (std::abs(diff) < 0.001f)
    {
        // Exactly on a measurement — no interpolation needed
        idxLower = idxUpper = nearest;
        alpha = 0.0f;
        return;
    }

    // Estimate bin spacing from dataset density
    const float binSpacing = 360.0f / static_cast<float>(m_numMeasurements);

    if (diff > 0.0f)
    {
        // az > az_near → nearest is the LOWER bin; search for the UPPER bin
        idxLower = nearest;
        float az_search = az_near + binSpacing;
        while (az_search >= 360.f) az_search -= 360.f;
        idxUpper = m_sofaReader->getNearestPositionIndex(az_search, elevationDeg);

        if (idxUpper == idxLower)
        {
            // Dataset has only one position in this range
            alpha = 0.0f;
            return;
        }

        float az_upper = m_sofaReader->getPositionAzimuth(idxUpper);
        float span = az_upper - az_near;
        while (span <= 0.f) span += 360.f;   // handle wrap-around
        alpha = juce::jlimit(0.0f, 1.0f, diff / span);
    }
    else
    {
        // az < az_near → nearest is the UPPER bin; search for the LOWER bin
        idxUpper = nearest;
        float az_search = az_near - binSpacing;
        while (az_search <    0.f) az_search += 360.f;
        idxLower = m_sofaReader->getNearestPositionIndex(az_search, elevationDeg);

        if (idxLower == idxUpper)
        {
            alpha = 0.0f;
            return;
        }

        float az_lower = m_sofaReader->getPositionAzimuth(idxLower);
        float span = az_near - az_lower;
        while (span <= 0.f) span += 360.f;
        alpha = juce::jlimit(0.0f, 1.0f, 1.0f + diff / span);
    }
}



void HRTFTable::resampleIR(const std::vector<float>& in,
                             std::vector<float>& out,
                             double ratio)
{
    const int inLen  = static_cast<int>(in.size());
    const int outLen = static_cast<int>(out.size());

    for (int i = 0; i < outLen; ++i)
    {
        const double pos  = static_cast<double>(i) / ratio;
        const int    idx  = static_cast<int>(pos);
        const double frac = pos - idx;

        if (idx < inLen - 1)
            out[i] = static_cast<float>((1.0 - frac) * in[idx] + frac * in[idx + 1]);
        else if (idx < inLen)
            out[i] = in[idx];
        else
            out[i] = 0.0f;
    }
}
