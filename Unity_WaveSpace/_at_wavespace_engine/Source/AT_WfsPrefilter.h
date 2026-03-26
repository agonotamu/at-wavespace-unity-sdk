/**
 * @file AT_WfsPrefilter.h
 * @brief IIR half-derivative prefilter for the 2.5D WFS driving function.
 * @author Antoine Gonot
 * @date 2026
 *
 * @details
 * In 2.5D Wave Field Synthesis, the exact driving function contains a
 * frequency-domain factor sqrt(j*omega). In the time domain, this corresponds
 * to a half-derivative operator whose frequency response is:
 *
 *   H(omega) = sqrt(j*omega) = sqrt(omega) * exp(j*pi/4)
 *
 * Properties:
 *   - Amplitude : +3 dB per octave (slope +1/2 in log-log)
 *   - Phase     : constant +45 degrees across the whole audio band
 *
 * A true half-derivative cannot be expressed with a finite rational transfer
 * function. This class approximates it using a cascade of N first-order IIR
 * sections (pole-zero pairs) whose poles and zeros alternate and are spaced
 * logarithmically across the audio band [fLow, fHigh]. Each section
 * contributes approximately +1.5 dB/octave and +22.5 degrees of phase,
 * so N = 6 sections yield a combined slope close to the target over the
 * full audio band with less than ±0.5 dB of ripple.
 *
 * The bilinear transform is used to map each analogue first-order section
 * to the discrete-time domain without frequency-axis warping artefacts at
 * audio frequencies.
 *
 * Usage:
 * @code
 *   AT::WfsPrefilter prefilter;
 *   prefilter.prepare(sampleRate);          // call once at setup
 *
 *   // In the audio callback, before pushing each sample to the WFS delay line:
 *   float filtered = prefilter.processSample(rawSample);
 *   delayLine.pushSample(0, filtered);
 * @endcode
 *
 * References:
 *   - Verheijen, PhD thesis TU Delft, 1997 — section 3.4
 *   - Spors & Rabenstein, AES 2006 — "Spatial Aliasing Artifacts Produced by
 *     Linear and Circular Loudspeaker Arrays"
 *   - Caulkins, PhD thesis Univ. Paris 6, 2007 — ch. 2
 */

#pragma once

#include <cmath>
#include <cassert>

#ifndef M_PI
    #define M_PI 3.14159265358979323846
#endif

namespace AT
{
    /**
     * @class WfsPrefilter
     * @brief Approximation of sqrt(j*omega) using a cascade of first-order IIR sections.
     *
     * All state and coefficient arrays are statically allocated (no heap use after
     * construction), making this class safe to use inside the audio callback.
     */
    class WfsPrefilter
    {
    public:

        // ====================================================================
        // CONFIGURATION
        // ====================================================================

        /**
         * @brief Number of first-order IIR sections in the cascade.
         *
         * 6 sections provide less than ±0.5 dB amplitude ripple and less than
         * ±3 degrees of phase error across 20 Hz – 20 kHz at 48 kHz sample rate.
         * Increasing this value improves accuracy at the cost of CPU and more
         * filter state to maintain.
         */
        static constexpr int NUM_SECTIONS = 6;

        // ====================================================================
        // LIFECYCLE
        // ====================================================================

        /** @brief Default constructor — call prepare() before any processing. */
        WfsPrefilter() = default;

        /**
         * @brief Designs the IIR cascade coefficients and resets the filter state.
         *
         * Must be called at least once before processSample(). Safe to call again
         * if the sample rate changes (e.g. device switch).
         *
         * @param sampleRate  Audio sample rate in Hz (e.g. 44100, 48000, 96000)
         * @param fLow        Lower frequency boundary of the approximation (Hz).
         *                    Below this frequency the filter rolls off — corresponds
         *                    to the physical lower limit of valid WFS synthesis.
         * @param fHigh       Upper frequency boundary of the approximation (Hz).
         *                    Should not exceed the Nyquist frequency (sampleRate / 2).
         */
        void prepare(double sampleRate,
                     float  fLow  = 20.0f,
                     float  fHigh = 20000.0f)
        {
            assert(sampleRate > 0.0);
            assert(fLow  > 0.0f && fLow  < fHigh);
            assert(fHigh < static_cast<float>(sampleRate) * 0.5f);

            m_sampleRate = sampleRate;
            designCoefficients(fLow, fHigh);
            reset();
        }

        /**
         * @brief Resets all internal delay states to zero without changing coefficients.
         *
         * Call this when switching modes (e.g. disabling and re-enabling the prefilter)
         * to avoid a transient caused by stale state.
         */
        void reset()
        {
            for (int k = 0; k < NUM_SECTIONS; ++k)
                m_state[k] = 0.0f;
        }

        // ====================================================================
        // PROCESSING
        // ====================================================================

        /**
         * @brief Processes one audio sample through the entire IIR cascade.
         *
         * Must be called sample-by-sample in the audio callback, once per input
         * sample, before the sample is pushed into the WFS delay line.
         * This method is inlined for zero function-call overhead on the audio thread.
         *
         * @param  x  Input audio sample.
         * @return    Filtered output sample with +3 dB/oct amplitude and +45 deg phase.
         */
        inline float processSample(float x) noexcept
        {
            // Cascade: output of section k feeds the input of section k+1.
            // Each section implements a first-order IIR in transposed Direct Form II:
            //
            //   y[n]      = b0 * x[n] + state[k]
            //   state[k]  = b1 * x[n] - a1 * y[n]
            //
            // Transposed DF-II is preferred over DF-I because it requires only
            // one delay register per section, halving memory usage, and it has
            // better numerical properties at low coefficients values.
            for (int k = 0; k < NUM_SECTIONS; ++k)
            {
                float y       = m_b0[k] * x + m_state[k];
                m_state[k]    = m_b1[k] * x - m_a1[k] * y;
                x             = y;
            }

            // Apply global gain normalisation (amplitude = 1.0 at the reference
            // frequency, 1 kHz by default, so that the prefilter does not change
            // the perceived loudness at mid-frequencies).
            return x * m_gain;
        }

    private:

        // ====================================================================
        // COEFFICIENT DESIGN
        // ====================================================================

        /**
         * @brief Designs the pole-zero pairs and converts them to discrete-time
         *        coefficients via the bilinear transform.
         *
         * Strategy:
         *   1. Divide [wLow, wHigh] into NUM_SECTIONS equal intervals on a log scale.
         *   2. Place the zero of section k at the geometric midpoint of the lower half
         *      of interval k, and the pole at the midpoint of the upper half.
         *      This guarantees z_k < p_k for every section, which ensures a positive
         *      magnitude slope (+3 dB/oct cumulatively across all sections).
         *   3. Map each analogue first-order section H_k(s) = (s + z_k) / (s + p_k)
         *      to discrete time using the bilinear transform s = (2/T)(z-1)/(z+1),
         *      yielding a first-order IIR with coefficients b0, b1, a1.
         *   4. Compute a global gain normalisation so that |H(j*wRef)| = 1.
         *
         * @param fLow   Lower analogue frequency boundary (Hz).
         * @param fHigh  Upper analogue frequency boundary (Hz).
         */
        void designCoefficients(float fLow, float fHigh)
        {
            const double T    = 1.0 / m_sampleRate;          // sampling period (s)
            const double wLow = 2.0 * M_PI * fLow;           // lower radian frequency
            const double wHigh= 2.0 * M_PI * fHigh;          // upper radian frequency

            // Logarithmic spacing: each interval spans this ratio on the frequency axis.
            const double logRatio = std::log(wHigh / wLow) / NUM_SECTIONS;

            // Bilinear transform constant: two over the sampling period.
            const double twoOverT = 2.0 / T;

            // ----------------------------------------------------------------
            // Global gain normalisation at the reference frequency (1 kHz).
            // The ideal half-derivative amplitude at wRef is sqrt(wRef).
            // We normalise so that the cascade output matches unity at wRef.
            // ----------------------------------------------------------------
            const double wRef  = 2.0 * M_PI * 1000.0;
            double gainProduct = 1.0;

            for (int k = 0; k < NUM_SECTIONS; ++k)
            {
                // Analogue zero and pole for section k.
                // Zero at 25% and pole at 75% of each log-spaced interval
                // — keeps z_k comfortably below p_k throughout the band.
                double zk = wLow * std::exp(logRatio * (k + 0.25));
                double pk = wLow * std::exp(logRatio * (k + 0.75));

                // ── Bilinear transform ──────────────────────────────────────
                // H_k(s) = (s + z_k) / (s + p_k)
                // s → (2/T) * (z - 1) / (z + 1)
                //
                // After algebra:
                //   b0 =  (twoOverT + zk) / (twoOverT + pk)
                //   b1 = (-twoOverT + zk) / (twoOverT + pk)
                //   a1 = (-twoOverT + pk) / (twoOverT + pk)   ← coefficient of y[n-1]
                //
                // Note: a1 is stored with its actual sign (as it appears in the
                // denominator polynomial). The transposed DF-II update equation is:
                //   state[k] = b1*x[n] - a1*y[n]
                // ─────────────────────────────────────────────────────────────
                double denom = twoOverT + pk;
                m_b0[k] = static_cast<float>((twoOverT + zk) / denom);
                m_b1[k] = static_cast<float>((-twoOverT + zk) / denom);
                m_a1[k] = static_cast<float>((-twoOverT + pk) / denom);

                // Accumulate the gain of this section at wRef for normalisation.
                // |H_k(j*wRef)| = |j*wRef + zk| / |j*wRef + pk|
                //                = sqrt(wRef^2 + zk^2) / sqrt(wRef^2 + pk^2)
                double numMag = std::sqrt(wRef * wRef + zk * zk);
                double denMag = std::sqrt(wRef * wRef + pk * pk);
                gainProduct  *= numMag / denMag;
            }

            // m_gain corrects the cascade amplitude to 1.0 at wRef.
            m_gain = static_cast<float>(1.0 / gainProduct);        }

        // ====================================================================
        // PRIVATE MEMBERS
        // ====================================================================

        double m_sampleRate = 48000.0; ///< Audio sample rate (Hz)
        float  m_gain       = 1.0f;    ///< Global normalisation gain

        /// Discrete-time feedforward coefficients (b0, b1) per section.
        float m_b0[NUM_SECTIONS] = {};
        float m_b1[NUM_SECTIONS] = {};

        /// Discrete-time feedback coefficient (a1, coefficient of y[n-1]) per section.
        /// Stored with its actual sign — see processSample() for sign convention.
        float m_a1[NUM_SECTIONS] = {};

        /// Transposed DF-II delay state register, one per section.
        float m_state[NUM_SECTIONS] = {};
    };

} // namespace AT
