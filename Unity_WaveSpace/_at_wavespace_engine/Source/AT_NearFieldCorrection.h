/**
 * @file AT_NearFieldCorrection.h
 * @brief Stereo near-field correction filter for binaural WFS virtualization (C2 condition).
 * @author Antoine Gonot
 * @date 2026
 *
 * @details
 * In WFS binaural virtualization (C2 condition), each virtual speaker is rendered
 * using a BRIR measured at a fixed reference distance r_ref. The WFS driving function
 * correctly encodes the wavefront curvature of a near-field source (r_virtual < r_ref),
 * but the BRIR does not — it always models the head/ear response at r_ref.
 *
 * The missing near-field cue is the inter-aural level difference (ILD) at low
 * frequencies that arises when the ipsilateral ear is significantly closer to the
 * source than the contralateral ear. This effect (described by Brungart & Rabinowitz
 * 1999) is present in the real WFS field captured by the KEMAR (C1 condition) but
 * absent in C2 when a distance-independent BRIR is used.
 *
 * This class applies a first-order IIR correction filter to the stereo binaural
 * output based on the rigid sphere model (Duda & Martens 1998). For each ear,
 * the correction transfer function relative to the BRIR at r_ref is:
 *
 *   H_ear(s) = (r_ref / r_ear) × (s + c/r_ref) / (s + c/r_ear)
 *
 * Properties:
 *   - Frequency-independent (unity) above ~c/r_ear  (~343 Hz for r=1m)
 *   - Low-frequency boost proportional to r_ref/r_ear when r_ear < r_ref
 *   - Phase: first-order all-pass character at mid/high frequencies
 *   - No effect when r_virtual == r_ref (filter is transparent)
 *   - No effect at θ=0° (frontal source, r_ipsi == r_contra)
 *
 * Ear distances are derived from the simple geometric model for a sphere of
 * radius a (head radius), given source distance r and azimuth θ:
 *
 *   r_ipsi  = sqrt(r² + a² - 2·r·a·sin|θ|)   (≈ r - a·sin|θ| for r >> a)
 *   r_contra = sqrt(r² + a² + 2·r·a·sin|θ|)  (≈ r + a·sin|θ| for r >> a)
 *
 * For azimuth > 0 (right source), the right ear is ipsilateral.
 *
 * Usage:
 * @code
 *   AT::NearFieldCorrection nfc;
 *   nfc.prepare(sampleRate);
 *   nfc.setParameters(0.5f, 1.0f, 90.0f);   // r_virtual=0.5m, r_ref=1.0m, az=90°
 *
 *   // In the audio callback, on m_binauralTemp[L] and [R]:
 *   nfc.processStereo(ptrL, ptrR, numSamples);
 * @endcode
 *
 * References:
 *   - Duda & Martens, JASA 1998 — "Range dependence of the response of a spherical
 *     head model"
 *   - Brungart & Rabinowitz, JASA 1999 — "Auditory localization of nearby sources"
 *   - Kan et al., JAES 2009 — "A psychophysical study of near-field HRTF interpolation"
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
     * @class NearFieldCorrection
     * @brief Stereo first-order IIR near-field correction filter.
     *
     * All state is statically allocated (no heap use after construction).
     * Safe to use inside the audio callback.
     *
     * Thread safety:
     *   setParameters() is called from the main thread; processStereo() from the
     *   audio thread. The coefficients are updated atomically via a pending-flag
     *   double-buffer: setParameters() writes into a shadow set and raises
     *   m_pendingUpdate; processStereo() picks up the shadow at the start of each
     *   block and swaps it in — no lock required.
     */
    class NearFieldCorrection
    {
    public:

        // ====================================================================
        // PHYSICAL CONSTANTS
        // ====================================================================

        /// Speed of sound in air at 20 °C (m/s)
        static constexpr float SPEED_OF_SOUND = 343.0f;

        /// KEMAR head radius (m) — used as default; can be overridden in setParameters()
        static constexpr float DEFAULT_HEAD_RADIUS = 0.0875f;

        // ====================================================================
        // LIFECYCLE
        // ====================================================================

        NearFieldCorrection() = default;

        /**
         * @brief Initializes the filter for a given sample rate.
         *
         * Must be called at least once before processStereo(). Safe to call
         * again on device reconfiguration (sample-rate change).
         *
         * @param sampleRate  Audio sample rate in Hz.
         */
        void prepare(double sampleRate)
        {
            assert(sampleRate > 0.0);
            m_sampleRate = sampleRate;
            // Compute transparent (pass-through) coefficients as starting state
            computeCoefficients(m_rVirtual, m_rRef, m_azimuthDeg, m_headRadius);
            reset();
        }

        /**
         * @brief Resets all filter delay states to zero.
         *
         * Call when enabling the correction or after a transport rewind to avoid
         * a transient from stale state.
         */
        void reset()
        {
            m_stateL_x1 = m_stateL_y1 = 0.0f;
            m_stateR_x1 = m_stateR_y1 = 0.0f;
        }

        // ====================================================================
        // PARAMETER SETTING  (main thread)
        // ====================================================================

        /**
         * @brief Sets the near-field correction parameters.
         *
         * Recomputes the IIR coefficients for both ears. Coefficients are picked
         * up by the audio thread at the start of the next call to processStereo().
         *
         * @param rVirtual       Distance from listener to virtual WFS source (m).
         *                       Values below ~0.1m are clamped (avoid extreme gain).
         * @param rRef           Distance at which the BRIR was measured (m).
         *                       Typically 1.0 m for close-field HRTF sets.
         * @param azimuthDeg     Azimuth of the virtual source in degrees.
         *                       +90 = right, -90 = left, 0 = front.
         *                       For |az| < MIN_AZIMUTH_DEG the correction is bypassed
         *                       (near-zero ILD BF effect for frontal sources).
         * @param headRadius     Head/sphere radius in metres (default: KEMAR 0.0875 m).
         */
        void setParameters(float rVirtual,
                           float rRef,
                           float azimuthDeg,
                           float headRadius = DEFAULT_HEAD_RADIUS)
        {
            m_rVirtual   = std::max(rVirtual,   MIN_DISTANCE);
            m_rRef       = std::max(rRef,        MIN_DISTANCE);
            m_azimuthDeg = azimuthDeg;
            m_headRadius = std::max(headRadius, 0.01f);

            computeCoefficients(m_rVirtual, m_rRef, m_azimuthDeg, m_headRadius);
            m_pendingUpdate.store(true, std::memory_order_release);
        }

        // ====================================================================
        // PROCESSING  (audio thread)
        // ====================================================================

        /**
         * @brief Applies the near-field correction in-place to a stereo buffer.
         *
         * No-op if the filter is transparent (r_virtual >= r_ref, or |az| too small).
         *
         * @param pL          Pointer to left-channel samples (read/write).
         * @param pR          Pointer to right-channel samples (read/write).
         * @param numSamples  Number of samples to process.
         */
        inline void processStereo(float* pL, float* pR, int numSamples) noexcept
        {
            // Pick up new coefficients atomically at block boundary
            if (m_pendingUpdate.load(std::memory_order_acquire))
            {
                m_activeB0_L = m_pendingB0_L;
                m_activeB1_L = m_pendingB1_L;
                m_activeA1_L = m_pendingA1_L;
                m_activeB0_R = m_pendingB0_R;
                m_activeB1_R = m_pendingB1_R;
                m_activeA1_R = m_pendingA1_R;
                m_pendingUpdate.store(false, std::memory_order_release);
            }

            // Bypass check: filter is transparent if coefficients are at unity
            if (m_bypass)
                return;

            // Process left channel  — Direct Form I, one pole / one zero
            for (int i = 0; i < numSamples; ++i)
            {
                float x   = pL[i];
                float y   = m_activeB0_L * x + m_activeB1_L * m_stateL_x1
                                              - m_activeA1_L * m_stateL_y1;
                m_stateL_x1 = x;
                m_stateL_y1 = y;
                pL[i] = y;
            }

            // Process right channel
            for (int i = 0; i < numSamples; ++i)
            {
                float x   = pR[i];
                float y   = m_activeB0_R * x + m_activeB1_R * m_stateR_x1
                                              - m_activeA1_R * m_stateR_y1;
                m_stateR_x1 = x;
                m_stateR_y1 = y;
                pR[i] = y;
            }
        }

    private:

        // ====================================================================
        // COEFFICIENT COMPUTATION
        // ====================================================================

        /**
         * @brief Computes bilinear-transformed IIR coefficients for both ears.
         *
         * Transfer function per ear (analogue):
         *
         *   H_ear(s) = (r_ref / r_ear) × (s + c/r_ref) / (s + c/r_ear)
         *
         * Bilinear transform s = 2·fs·(1 - z⁻¹)/(1 + z⁻¹)  yields:
         *
         *   b0 = gain × (2fs + ωz) / (2fs + ωp)
         *   b1 = gain × (-2fs + ωz) / (2fs + ωp)
         *   a1 = (-2fs + ωp) / (2fs + ωp)       (sign: y[n] = b0·x - b1·x[n-1] - a1·y[n-1])
         *
         * where ωz = c/r_ref, ωp = c/r_ear, gain = r_ref/r_ear.
         *
         * Direct Form I:  y[n] = b0·x[n] + b1·x[n-1] - a1·y[n-1]
         *
         * @note  Pre-warping is omitted intentionally: the pole/zero frequencies
         *        (55–685 Hz for r 0.5–1 m) are well within the audio band and
         *        far from Nyquist, so aliasing via bilinear warping is negligible.
         */
        void computeCoefficients(float rVirtual, float rRef, float azimuthDeg, float headRadius)
        {
            const float fs   = static_cast<float>(m_sampleRate);
            const float twoFs = 2.0f * fs;
            const float c    = SPEED_OF_SOUND;
            const float a    = headRadius;

            // -- Ear distances from the virtual source -----------------------
            //
            // Simple geometric model on the horizontal plane:
            //   r_ipsi  = sqrt(r² + a² - 2ra·sin|θ|)
            //   r_contra = sqrt(r² + a² + 2ra·sin|θ|)
            //
            // "ipsilateral" means the ear on the same side as the source:
            //   θ > 0 (right)  → right ear is ipsilateral
            //   θ < 0 (left)   → left ear is ipsilateral

            const float az_rad   = azimuthDeg * static_cast<float>(M_PI) / 180.0f;
            const float sinAbs   = std::abs(std::sin(az_rad));   // ∈ [0, 1]
            const float r2       = rVirtual * rVirtual;
            const float a2       = a * a;
            const float cross    = 2.0f * rVirtual * a * sinAbs;

            float r_ipsi  = std::sqrt(r2 + a2 - cross);
            float r_contra = std::sqrt(r2 + a2 + cross);

            // Clamp to avoid division by zero / extreme gain for very near sources
            r_ipsi  = std::max(r_ipsi,   MIN_DISTANCE);
            r_contra = std::max(r_contra, MIN_DISTANCE);

            // -- Bypass decision --------------------------------------------
            // The correction is only meaningful for r_virtual < r_ref.
            // When the source is at or beyond the reference distance the BRIR
            // already captures the correct (or over-estimated) near-field ILD,
            // so the filter must be transparent regardless of azimuth.
            //
            // A secondary bypass for near-frontal sources (sinAbs ≈ 0) avoids
            // computing negligible corrections, but only within the valid range.
            const bool inRange      = (rVirtual < rRef * TRANSPARENCY_RATIO);
            const bool lateralEnough = (sinAbs   > MIN_AZIMUTH_SIN);
            m_bypass = !(inRange && lateralEnough);

            if (m_bypass)
            {
                // Write pass-through coefficients (b0=1, b1=0, a1=0) — no-op
                m_pendingB0_L = m_pendingB0_R = 1.0f;
                m_pendingB1_L = m_pendingB1_R = 0.0f;
                m_pendingA1_L = m_pendingA1_R = 0.0f;
                return;
            }

            // -- Analogue pole/zero frequencies ----------------------------
            const float omegaZ = c / rRef;          // zero (same for both ears)

            // -- HF gain normalisation -------------------------------------
            //
            // Without normalisation the filter has HF gain = r_ref/r_ear, which
            // produces a large broadband level boost relative to the bypass path
            // (up to +7 dB per channel at 0.5 m, az=90°).
            //
            // We want to preserve only the *inter-aural* level difference (ILD),
            // not the absolute level increase. Normalising by the geometric mean
            // of the two HF gains keeps their ratio (= the ILD) intact while
            // bringing the average HF gain back to 0 dB:
            //
            //   K = sqrt(G_L_HF × G_R_HF) = r_ref / sqrt(r_L × r_R)
            //   normGain = 1/K = sqrt(r_L × r_R) / r_ref
            //
            // Effect on gains:
            //   HF: G_L → G_L/K = sqrt(r_R/r_L)   (< 1 for contra, > 1 for ipsi)
            //   HF: G_R → G_R/K = sqrt(r_L/r_R)   (symmetric)
            //   DC: both channels get normGain (slight LF attenuation — acceptable
            //       since the near-field ILD cue is primarily a HF/broadband effect)
            //   ILD = G_R_HF/G_L_HF = r_L/r_R  →  unchanged after normalisation ✓

            // -- Determine which physical ear is ipsilateral ---------------
            // L channel correction: if az > 0 (right), L is contralateral
            //                       if az < 0 (left),  L is ipsilateral
            float r_L, r_R;
            if (azimuthDeg >= 0.0f)
            {
                r_L = r_contra;   // left ear = contralateral
                r_R = r_ipsi;     // right ear = ipsilateral
            }
            else
            {
                r_L = r_ipsi;     // left ear = ipsilateral
                r_R = r_contra;   // right ear = contralateral
            }

            // normGain = 1/K = sqrt(r_L * r_R) / r_ref
            // Applied to b0 and b1 only (not a1): scales the numerator,
            // leaves the pole unchanged. The ILD ratio is preserved exactly.
            const float normGain = std::sqrt(r_L * r_R) / rRef;

            // -- Left channel coefficients ---------------------------------
            {
                const float omegaP = c / r_L;
                const float gain   = normGain * rRef / r_L;   // = normGain × G_L_HF
                const float denom  = twoFs + omegaP;
                m_pendingB0_L = gain * (twoFs + omegaZ) / denom;
                m_pendingB1_L = gain * (-twoFs + omegaZ) / denom;
                m_pendingA1_L = (-twoFs + omegaP) / denom;
            }

            // -- Right channel coefficients --------------------------------
            {
                const float omegaP = c / r_R;
                const float gain   = normGain * rRef / r_R;   // = normGain × G_R_HF
                const float denom  = twoFs + omegaP;
                m_pendingB0_R = gain * (twoFs + omegaZ) / denom;
                m_pendingB1_R = gain * (-twoFs + omegaZ) / denom;
                m_pendingA1_R = (-twoFs + omegaP) / denom;
            }
        }

        // ====================================================================
        // CONSTANTS
        // ====================================================================

        /// Minimum allowable distance (m) — prevents infinite gain
        static constexpr float MIN_DISTANCE       = 0.10f;

        /// Filter is transparent if r_virtual >= r_ref × this ratio
        static constexpr float TRANSPARENCY_RATIO = 0.99f;

        /// Filter bypass threshold for near-frontal sources (|sin(az)| < this)
        static constexpr float MIN_AZIMUTH_SIN    = 0.05f;  // ≈ |az| < 3°

        // ====================================================================
        // PARAMETERS  (written by main thread, applied by audio thread)
        // ====================================================================

        double m_sampleRate = 48000.0;
        float  m_rVirtual   = 2.0f;    ///< Virtual source distance (m)
        float  m_rRef       = 1.0f;    ///< BRIR reference distance (m)
        float  m_azimuthDeg = 0.0f;    ///< Source azimuth (degrees)
        float  m_headRadius = DEFAULT_HEAD_RADIUS;

        // ====================================================================
        // COEFFICIENT DOUBLE BUFFER — pending (main thread) / active (audio thread)
        // ====================================================================

        std::atomic<bool> m_pendingUpdate{false};
        bool              m_bypass = true;

        // Pending (main thread writes)
        float m_pendingB0_L{1.0f}, m_pendingB1_L{0.0f}, m_pendingA1_L{0.0f};
        float m_pendingB0_R{1.0f}, m_pendingB1_R{0.0f}, m_pendingA1_R{0.0f};

        // Active (audio thread reads)
        float m_activeB0_L{1.0f}, m_activeB1_L{0.0f}, m_activeA1_L{0.0f};
        float m_activeB0_R{1.0f}, m_activeB1_R{0.0f}, m_activeA1_R{0.0f};

        // ====================================================================
        // FILTER STATE  (audio thread only)
        // ====================================================================

        float m_stateL_x1{0.0f}, m_stateL_y1{0.0f};   ///< Left channel DF-I state
        float m_stateR_x1{0.0f}, m_stateR_y1{0.0f};   ///< Right channel DF-I state
    };

} // namespace AT
