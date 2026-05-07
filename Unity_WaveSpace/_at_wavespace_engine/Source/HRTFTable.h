#pragma once

#include "SOFAReader.h"     // pulls in JuceHeader.h → juce_dsp
#include <vector>
#include <memory>
#include <cassert>

/**
 * @file HRTFTable.h
 * @brief Shared HRTF data table — raw IRs + pre-computed FFTs for OLA convolution.
 * @author Antoine Gonot  @date 2026
 *
 * Two life-cycle stages:
 *
 *   1. build()       — load + resample + truncate IRs from a SOFAReader.
 *                      No FFT yet; block size is not known at this point.
 *
 *   2. prepareFFT()  — compute FFT(h[m]) for all measurements m once the
 *                      audio block size is known (called in prepareToPlay).
 *                      FFT size = nextPow2(blockSize + irLength - 1).
 *
 * Runtime (per audio block per channel):
 *   HRTFProcessor calls getFFT_L/R(currentIndex) — O(1) pointer lookup.
 *   OLA: 1 forward FFT + 2 complex multiplies + 2 inverse FFTs per block.
 *
 * Complexity for N=2048, M=1024 (FFT_SIZE=4096):
 *   Direct FIR  : 2048 × 1024 = 2,097,152 MACs / channel / block
 *   FFT OLA     : ≈ 3 × 4096 × 12 =   147,456 ops / channel / block  (14× faster)
 */
class HRTFTable
{
public:
    static constexpr int MAX_IR_LENGTH = 1024;

    HRTFTable() = default;

    // ── Stage 1 ───────────────────────────────────────────────────────────────

    /**
     * @brief Builds the raw IR table from a loaded SOFAReader.
     * Resamples to audioSampleRate and truncates to min(maxIRLength, MAX_IR_LENGTH).
     * Does NOT compute FFTs — call prepareFFT() after knowing the block size.
     */
    bool build(const SOFAReader& reader, double audioSampleRate, int maxIRLength = 0);

    // ── Stage 2 ───────────────────────────────────────────────────────────────

    /**
     * @brief Pre-computes FFT(h[m]) for all measurements at the given block size.
     * FFT size = nextPow2(blockSize + irLength - 1).
     * Idempotent: no-op if already prepared for the same blockSize.
     * Must be called from the main thread (prepareToPlay).
     */
    void prepareFFT(int blockSize);

    // ── State ─────────────────────────────────────────────────────────────────

    bool isBuilt()           const { return m_built; }
    bool isFFTPrepared()     const { return m_fftPrepared; }
    int  getNumMeasurements()const { return m_numMeasurements; }
    int  getIRLength()       const { return m_irLength; }
    int  getFFTSize()        const { return m_fftSize; }
    int  getFFTOrder()       const { return m_fftOrder; }

    // ── Runtime access (audio thread, O(1)) ───────────────────────────────────

    /**
     * @brief Pre-computed complex FFT of left IR for measurement m.
     * Size: 2 * getFFTSize() floats, JUCE interleaved [re0,im0, re1,im1, …].
     */
    const float* getFFT_L(int m) const noexcept
    {
        assert(m_fftPrepared && m >= 0 && m < m_numMeasurements);
        return m_fftData.data() + static_cast<size_t>(m) * 2 * 2 * m_fftSize;
    }

    /**
     * @brief Pre-computed complex FFT of right IR for measurement m.
     */
    const float* getFFT_R(int m) const noexcept
    {
        assert(m_fftPrepared && m >= 0 && m < m_numMeasurements);
        return m_fftData.data() + static_cast<size_t>(m) * 2 * 2 * m_fftSize + 2 * m_fftSize;
    }

    /**
     * @brief Shared juce::dsp::FFT instance.
     * juce::dsp::FFT is stateless (read-only twiddle factors) → thread-safe.
     */
    const juce::dsp::FFT& getFFT() const
    {
        assert(m_fftPrepared);
        return *m_fft;
    }

    // ── Position lookup ───────────────────────────────────────────────────────

    int getNearestIndex(float azimuthDeg, float elevationDeg) const
    {
        return m_sofaReader->getNearestPositionIndex(azimuthDeg, elevationDeg);
    }

    /**
     * @brief Finds the two SOFA measurements that bracket the given azimuth
     *        and computes the interpolation factor alpha in [0, 1].
     *
     * H_blend = (1 - alpha) × H[idxLower] + alpha × H[idxUpper]
     *
     * For uniform-spacing HRTF datasets (e.g. KEMAR at 2.5°), this gives
     * continuous HRTF variation as the azimuth changes smoothly — no discrete
     * bin switching, no clicks on fast listener/speaker movement.
     *
     * If the two nearest bins are the same (exact bin match, or single-position
     * dataset), idxLower == idxUpper and alpha = 0.
     *
     * @param azimuthDeg  Query azimuth in degrees.
     * @param elevationDeg Query elevation (0 for horizontal WFS arrays).
     * @param idxLower    Output: index of the measurement just below az.
     * @param idxUpper    Output: index of the measurement just above az.
     * @param alpha       Output: fractional position between lower and upper [0,1].
     */
    void findBracketingIndices(float azimuthDeg, float elevationDeg,
                                int& idxLower, int& idxUpper, float& alpha) const;

    // ── Rebuild support (setHrtfTruncate) ─────────────────────────────────────

    const SOFAReader& getSOFAReader() const { return *m_sofaReader; }

private:

    // Stage-1 state
    bool   m_built           = false;
    int    m_numMeasurements = 0;
    int    m_irLength        = 0;

    // Raw IRs (original order) — layout: [meas0_L[M] | meas0_R[M] | meas1_L | …]
    std::vector<float> m_data;

    // Retained for position lookup and rebuild
    std::shared_ptr<SOFAReader> m_sofaReader;

    // Stage-2 state
    bool   m_fftPrepared  = false;
    int    m_fftSize      = 0;
    int    m_fftOrder     = 0;
    int    m_fftBlockSize = 0;

    // Pre-computed FFTs — layout: [meas0_L[2*fftSize] | meas0_R[2*fftSize] | …]
    std::vector<float> m_fftData;

    std::unique_ptr<juce::dsp::FFT> m_fft;

    static void resampleIR(const std::vector<float>& in,
                            std::vector<float>& out,
                            double ratio);
};
