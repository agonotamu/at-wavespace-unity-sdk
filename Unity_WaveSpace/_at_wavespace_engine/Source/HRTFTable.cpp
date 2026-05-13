#include "HRTFTable.h"
#include <algorithm>
#include <cstring>
#include <cmath>

// ── Move assignment ───────────────────────────────────────────────────────────
//
// The naive compiler-generated move assignment would:
//  (a) free the old m_fftData while audio threads may hold raw pointers into it, and
//  (b) move (and potentially destroy) m_fftDataMutex while a shared_lock is active.
//
// The custom implementation acquires the exclusive lock BEFORE touching any data,
// so tryBlendHRTF() callers finish reading before the old storage is released.
// m_fftDataMutex is intentionally NOT moved — it stays bound to this object.

HRTFTable& HRTFTable::operator=(HRTFTable&& other) noexcept
{
    if (this == &other) return *this;

    // Block until every audio-thread shared_lock on this table's mutex is released.
    std::unique_lock<std::shared_mutex> lock(*m_fftDataMutex);

    // Move all data members — the old values are destroyed here, safely under lock.
    m_built           = other.m_built;
    m_numMeasurements = other.m_numMeasurements;
    m_irLength        = other.m_irLength;
    m_data            = std::move(other.m_data);
    m_sofaReader      = std::move(other.m_sofaReader);
    m_fftPrepared     = other.m_fftPrepared;
    m_fftSize         = other.m_fftSize;
    m_fftOrder        = other.m_fftOrder;
    m_fftBlockSize    = other.m_fftBlockSize;
    m_fftData         = std::move(other.m_fftData);   // old m_fftData freed here, under lock
    m_fft             = std::move(other.m_fft);

    // m_fftDataMutex is intentionally left as-is: audio threads that were
    // waiting on it will wake up and find the new data already in place.
    return *this;
}



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

    // Exclusive lock: any concurrent tryBlendHRTF() call will fail its
    // try_lock_shared() and return false (silent block) for the duration
    // of this rebuild.  The lock is held until all writes to m_fftData finish.
    std::unique_lock<std::shared_mutex> lock(*m_fftDataMutex);

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

bool HRTFTable::tryBlendHRTF(int idxLower, int idxUpper, float alpha,
                               float* dstL, float* dstR, int bufSize) const noexcept
{
    // Non-blocking shared lock — returns false immediately if prepareFFT() holds
    // the exclusive lock (m_fftData is being reallocated).
    std::shared_lock<std::shared_mutex> lock(*m_fftDataMutex, std::try_to_lock);
    if (!lock) return false;

    // Validate indices under the lock (m_numMeasurements is stable while locked).
    if (!m_fftPrepared
        || idxLower < 0 || idxLower >= m_numMeasurements
        || idxUpper < 0 || idxUpper >= m_numMeasurements)
        return false;

    const float* hL_lo = getFFT_L(idxLower);
    const float* hL_hi = getFFT_L(idxUpper);
    const float* hR_lo = getFFT_R(idxLower);
    const float* hR_hi = getFFT_R(idxUpper);
    const float  oneMinusAlpha = 1.0f - alpha;

    // LERP in the frequency domain — auto-vectorised with -O2 (NEON/AVX)
    for (int k = 0; k < bufSize; ++k)
    {
        dstL[k] = oneMinusAlpha * hL_lo[k] + alpha * hL_hi[k];
        dstR[k] = oneMinusAlpha * hR_lo[k] + alpha * hR_hi[k];
    }

    return true;
}

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
