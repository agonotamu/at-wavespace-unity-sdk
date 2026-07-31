/**
 * @file AT_SixDofMaskProcessor.cpp
 * @author Antoine Gonot / Claude
 * @date 2026
 */

#include "AT_SixDofMaskProcessor.h"
#include <cmath>
#include <algorithm>
#include <iostream>

// ── TEMPORARY diagnostic logging — console app only, plain std::cout (no
// Unity/LOG() dependency). Flip to 0 to silence once diagnosis is done;
// left in place (not deleted) so it can be re-enabled without rewriting.
#define AT_SIXDOF_DEBUG_LOG 0

namespace AT
{
    // ====================================================================
    // Small 4x4 linear solve (Gaussian elimination with partial pivoting).
    // Used by sphericalInterpolation() — avoids pulling in a linear-algebra
    // dependency (Eigen etc.) for a 4-unknown system.
    // ====================================================================
    namespace
    {
        bool solve4x4(double A[4][4], double b[4], double outX[4])
        {
            // Augmented matrix Gaussian elimination, partial pivoting.
            for (int col = 0; col < 4; ++col)
            {
                int pivotRow = col;
                double maxVal = std::abs(A[col][col]);
                for (int row = col + 1; row < 4; ++row)
                {
                    if (std::abs(A[row][col]) > maxVal)
                    {
                        maxVal = std::abs(A[row][col]);
                        pivotRow = row;
                    }
                }
                if (maxVal < 1e-12)
                    return false; // singular / degenerate geometry

                if (pivotRow != col)
                {
                    std::swap(A[col], A[pivotRow]);
                    std::swap(b[col], b[pivotRow]);
                }

                for (int row = col + 1; row < 4; ++row)
                {
                    const double factor = A[row][col] / A[col][col];
                    for (int k = col; k < 4; ++k)
                        A[row][k] -= factor * A[col][k];
                    b[row] -= factor * b[col];
                }
            }

            for (int row = 3; row >= 0; --row)
            {
                double sum = b[row];
                for (int k = row + 1; k < 4; ++k)
                    sum -= A[row][k] * outX[k];
                outX[row] = sum / A[row][row];
            }
            return true;
        }
    }

    // ====================================================================
    // Construction / lifecycle
    // ====================================================================
    SixDofMaskProcessor::SixDofMaskProcessor() = default;

    SixDofMaskProcessor::~SixDofMaskProcessor()
    {
        releaseResources();
    }

    void SixDofMaskProcessor::prepare(int numChannels, double sampleRate, int maxBlockSize,
                                       const float* micPositionsFlat)
    {
        releaseResources(); // safe if never prepared

        m_numChannels = numChannels;
        m_sampleRate  = sampleRate;
        m_maxBlockSize = juce::jlimit(1, MAX_BLOCK_SIZE_SUPPORTED, maxBlockSize);

#if AT_SIXDOF_DEBUG_LOG
        std::cout << "[6DOF][prepare] numChannels=" << numChannels
                   << "  sampleRate=" << sampleRate
                   << "  maxBlockSize(requested)=" << maxBlockSize
                   << "  maxBlockSize(clamped)=" << m_maxBlockSize << "\n";
#endif

        m_micPositions.resize((size_t) numChannels);
        m_arrayCentroid = { 0.0f, 0.0f, 0.0f };
        for (int i = 0; i < numChannels; ++i)
        {
            m_micPositions[(size_t) i] = { micPositionsFlat[i * 3 + 0],
                                            micPositionsFlat[i * 3 + 1],
                                            micPositionsFlat[i * 3 + 2] };
            m_arrayCentroid[0] += m_micPositions[(size_t) i][0];
            m_arrayCentroid[1] += m_micPositions[(size_t) i][1];
            m_arrayCentroid[2] += m_micPositions[(size_t) i][2];
        }
        if (numChannels > 0)
        {
            m_arrayCentroid[0] /= (float) numChannels;
            m_arrayCentroid[1] /= (float) numChannels;
            m_arrayCentroid[2] /= (float) numChannels;
        }

        float maxExtent = 0.0f;
        for (int i = 0; i < numChannels; ++i)
        {
            const float dx = m_micPositions[(size_t) i][0] - m_arrayCentroid[0];
            const float dy = m_micPositions[(size_t) i][1] - m_arrayCentroid[1];
            const float dz = m_micPositions[(size_t) i][2] - m_arrayCentroid[2];
            maxExtent = std::max(maxExtent, std::sqrt(dx * dx + dy * dy + dz * dz));
        }
        // Matches the Python prototype's max_plausible_dist / MAX_TDOA_SECONDS
        // safety margins (see 6dof-nav skill) — generous enough to never
        // reject a genuine in-room source, tight enough to catch numerical
        // divergence on near-silent/noisy windows.
        m_maxPlausibleDist = std::max(1.0f, 10.0f * maxExtent);
        m_maxDelaySeconds  = (double) (2.0f * maxExtent / SPEED_OF_SOUND) * 1.5;

        // Rolling window buffer: sized for the largest configuration we'll
        // ever be asked for (MAX_BUFFERED_BLOCKS_SUPPORTED * maxBlockSize) so
        // that setNumBufferedBlocks() can change at runtime without
        // reallocating on the audio thread.
        m_windowBuffer.assign((size_t) numChannels,
            std::vector<float>((size_t) (m_maxBlockSize * MAX_BUFFERED_BLOCKS_SUPPORTED), 0.0f));
        m_windowWritePos = 0;
        m_windowTargetSamples = m_maxBlockSize * juce::jlimit(1, MAX_BUFFERED_BLOCKS_SUPPORTED,
                                                                m_numBufferedBlocks.load());

        m_pendingWindow.assign((size_t) numChannels, std::vector<float>());

        m_estimateHistory.assign(HISTORY_SIZE, SixDofSourcePosition{});
        m_historyWritePos = 0;
        m_historyCount    = 0;

        m_detectedSources.clear();
        m_trackedCandidates.clear();
        m_state.store(State::Bypass, std::memory_order_relaxed);
        m_staleCycleCount = 0;
        m_maskLogInitialized = false;
        m_lastLoggedSourcesForMaskLog.clear();

        m_channelGainSmoothers.clear();
        m_channelGainSmoothers.resize((size_t) numChannels);
        for (auto& smoother : m_channelGainSmoothers)
        {
            smoother.reset(sampleRate, GAIN_SMOOTH_TIME_SECONDS);
            smoother.setCurrentAndTargetValue(1.0f); // start in pass-through, matches Bypass state
        }

        // FFT sized for linear (non-circular) cross-correlation of two
        // windows of up to (maxBlockSize * MAX_BUFFERED_BLOCKS_SUPPORTED)
        // samples each: need size >= 2x that, rounded up to a power of two.
        const int maxWindowSamples = m_maxBlockSize * MAX_BUFFERED_BLOCKS_SUPPORTED;
        int order = 1;
        while ((1 << order) < maxWindowSamples * 2)
            ++order;
        m_fftOrder = order;
        m_fftSize  = 1 << order;
        m_fft = std::make_unique<juce::dsp::FFT>(m_fftOrder);
        m_fftBufA.assign((size_t) m_fftSize * 2, 0.0f);
        m_fftBufB.assign((size_t) m_fftSize * 2, 0.0f);

        m_shouldStop.store(false, std::memory_order_relaxed);
        m_backgroundThread = std::thread([this] { backgroundThreadLoop(); });
    }

    void SixDofMaskProcessor::releaseResources()
    {
        m_shouldStop.store(true, std::memory_order_relaxed);
        m_pendingWindowCv.notify_all();
        if (m_backgroundThread.joinable())
            m_backgroundThread.join();

        m_fft.reset();
        m_windowBuffer.clear();
        m_pendingWindow.clear();
        m_micPositions.clear();
        m_channelGainSmoothers.clear();
        m_numChannels = 0;
    }

    // ====================================================================
    // Parameters
    // ====================================================================
    void SixDofMaskProcessor::setEnabled(bool enabled)
    {
        m_enabled.store(enabled, std::memory_order_relaxed);
    }

    void SixDofMaskProcessor::setGridRes(float gridRes)
    {
        m_gridRes.store(std::max(0.001f, gridRes), std::memory_order_relaxed);
    }

    void SixDofMaskProcessor::setMinBlockCount(int minBlockCount)
    {
        m_minBlockCount.store(std::max(1, minBlockCount), std::memory_order_relaxed);
    }

    void SixDofMaskProcessor::setNumBufferedBlocks(int numBufferedBlocks)
    {
        const int clamped = juce::jlimit(1, MAX_BUFFERED_BLOCKS_SUPPORTED, numBufferedBlocks);
        m_numBufferedBlocks.store(clamped, std::memory_order_relaxed);
        // m_windowTargetSamples is re-latched at the START of the next window
        // by the audio thread (see process()) — never mutated mid-window.
    }

    // ====================================================================
    // Audio-thread entry points
    // ====================================================================
    void SixDofMaskProcessor::pushAnalysisBlock(const juce::AudioBuffer<float>& sourceBuffer, int startSample,
                                                  int numSamples, int numChannelsToProcess)
    {
        if (!m_enabled.load(std::memory_order_relaxed) || m_numChannels == 0)
            return;

        numChannelsToProcess = std::min(numChannelsToProcess, m_numChannels);

        // Latch the target window length at the start of a fresh window only
        // (mid-window parameter changes take effect on the NEXT window, to
        // avoid a partially-mixed window length).
        if (m_windowWritePos == 0)
            m_windowTargetSamples = m_maxBlockSize * m_numBufferedBlocks.load(std::memory_order_relaxed);

        const int samplesToCopy = std::min(numSamples, m_windowTargetSamples - m_windowWritePos);
        if (samplesToCopy > 0)
        {
            for (int ch = 0; ch < numChannelsToProcess; ++ch)
            {
                const float* src = sourceBuffer.getReadPointer(ch, startSample);
                std::copy(src, src + samplesToCopy, m_windowBuffer[(size_t) ch].begin() + m_windowWritePos);
            }
            m_windowWritePos += samplesToCopy;
        }

        if (m_windowWritePos >= m_windowTargetSamples)
        {
            // Hand off a copy to the background thread (non-blocking: if
            // it's still busy with the previous window, this one is simply
            // dropped — the real-time thread must never wait on the worker;
            // detection latency degrades gracefully instead of blocking audio).
            std::unique_lock<std::mutex> lock(m_pendingWindowMutex, std::try_to_lock);
            if (lock.owns_lock() && !m_pendingWindowReady)
            {
                for (int ch = 0; ch < numChannelsToProcess; ++ch)
                {
                    m_pendingWindow[(size_t) ch].assign(
                        m_windowBuffer[(size_t) ch].begin(),
                        m_windowBuffer[(size_t) ch].begin() + m_windowWritePos);
                }
                m_pendingWindowReady = true;
                lock.unlock();
                m_pendingWindowCv.notify_one();
            }
            m_windowWritePos = 0;
        }
    }

    void SixDofMaskProcessor::updateTargetGains(float listenerX, float listenerY, float listenerZ,
                                                  int numChannelsToProcess)
    {
        if (!m_enabled.load(std::memory_order_relaxed) || m_numChannels == 0)
            return;

        numChannelsToProcess = std::min(numChannelsToProcess, m_numChannels);
        const State state = m_state.load(std::memory_order_relaxed);

        if (state == State::Bypass)
        {
            for (int ch = 0; ch < numChannelsToProcess; ++ch)
                m_channelGainSmoothers[(size_t) ch].setTargetValue(1.0f);
            return;
        }

        std::lock_guard<std::mutex> lock(m_sourcesMutex);

#if AT_SIXDOF_DEBUG_LOG
        // Log only when the listener position or the detected source list
        // actually changed since the last log (updateTargetGains() runs
        // once per audio block — far more often than localization updates —
        // so logging unconditionally here would flood the console).
        bool changed = !m_maskLogInitialized;
        if (!changed)
        {
            const float EPS = 0.01f; // metres
            if (std::abs(listenerX - m_lastLoggedListenerX) > EPS ||
                std::abs(listenerY - m_lastLoggedListenerY) > EPS ||
                std::abs(listenerZ - m_lastLoggedListenerZ) > EPS)
                changed = true;
            if (!changed && m_detectedSources.size() != m_lastLoggedSourcesForMaskLog.size())
                changed = true;
            if (!changed)
            {
                for (size_t i = 0; i < m_detectedSources.size(); ++i)
                {
                    const auto& a = m_detectedSources[i];
                    const auto& b = m_lastLoggedSourcesForMaskLog[i];
                    if (std::abs(a.x - b.x) > EPS || std::abs(a.y - b.y) > EPS || std::abs(a.z - b.z) > EPS)
                    { changed = true; break; }
                }
            }
        }

        if (changed)
        {
            std::cout << "[6DOF][mask] listener=(" << listenerX << ", " << listenerY << ", "
                       << listenerZ << ")  state=" << (state == State::Valid ? "Valid" : "Stale")
                       << "  sources=" << m_detectedSources.size() << "\n";

            // Per-source breakdown: which channels THIS source alone would
            // validate (weight > 0.5), independent of the others — lets you
            // check each source's mask individually, e.g. that it roughly
            // covers the array's "far side" relative to the listener.
            for (size_t s = 0; s < m_detectedSources.size(); ++s)
            {
                const auto& src = m_detectedSources[s];
                std::vector<int> activeChannels;
                for (int ch = 0; ch < numChannelsToProcess; ++ch)
                    if (computeChannelWeight(ch, src, listenerX, listenerY, listenerZ) > 0.5f)
                        activeChannels.push_back(ch);

                std::cout << "[6DOF][mask]   source " << s << " @ (" << src.x << ", " << src.y << ", "
                           << src.z << ")  active_channels=" << activeChannels.size()
                           << "/" << numChannelsToProcess << "  [";
                for (size_t k = 0; k < activeChannels.size() && k < 20; ++k)
                    std::cout << activeChannels[k] << (k + 1 < activeChannels.size() ? "," : "");
                if (activeChannels.size() > 20) std::cout << ",...";
                std::cout << "]\n";
            }

            // Combined (final) mask actually applied to the audio — the max
            // across all sources, exactly as computed in the loop below.
            int combinedActive = 0;
            for (int ch = 0; ch < numChannelsToProcess; ++ch)
            {
                float w = 0.0f;
                for (const auto& src : m_detectedSources)
                    w = std::max(w, computeChannelWeight(ch, src, listenerX, listenerY, listenerZ));
                if (w > 0.5f) ++combinedActive;
            }
            std::cout << "[6DOF][mask]   combined (max across sources): " << combinedActive
                       << "/" << numChannelsToProcess << " channels active\n";

            m_lastLoggedListenerX = listenerX;
            m_lastLoggedListenerY = listenerY;
            m_lastLoggedListenerZ = listenerZ;
            m_lastLoggedSourcesForMaskLog = m_detectedSources;
            m_maskLogInitialized = true;
        }
#endif

        for (int ch = 0; ch < numChannelsToProcess; ++ch)
        {
            float weight = 0.0f;
            for (const auto& src : m_detectedSources)
                weight = std::max(weight, computeChannelWeight(ch, src, listenerX, listenerY, listenerZ));
            m_channelGainSmoothers[(size_t) ch].setTargetValue(weight);
        }
    }

    float SixDofMaskProcessor::getNextChannelGain(int channel)
    {
        if (!m_enabled.load(std::memory_order_relaxed) || channel < 0 || channel >= m_numChannels)
            return 1.0f;
        return m_channelGainSmoothers[(size_t) channel].getNextValue();
    }

    float SixDofMaskProcessor::computeChannelWeight(int channel, const SixDofSourcePosition& source,
                                                      float lx, float ly, float lz) const
    {
        // Direct port of mask_weights() in apply_mask.py: a microphone is
        // valid (weight -> 1) iff the source lies between the listener and
        // that microphone; invalid (weight -> 0) otherwise, with a
        // MASK_TRANSITION-metre continuous ramp instead of a hard cutoff
        // (avoids zipper artifacts as the listener moves — see the Python
        // prototype discussion on this exact point).
        const float vsx = source.x - lx, vsy = source.y - ly, vsz = source.z - lz;
        const float distSrc = std::sqrt(vsx * vsx + vsy * vsy + vsz * vsz);
        if (distSrc < 1e-6f)
            return 1.0f; // degenerate: listener essentially AT the source

        const float dirX = vsx / distSrc, dirY = vsy / distSrc, dirZ = vsz / distSrc;

        const auto& mic = m_micPositions[(size_t) channel];
        const float vmx = mic[0] - lx, vmy = mic[1] - ly, vmz = mic[2] - lz;
        const float proj = vmx * dirX + vmy * dirY + vmz * dirZ;

        const float w = (proj - distSrc) / MASK_TRANSITION;
        return juce::jlimit(0.0f, 1.0f, w);
    }

    // ====================================================================
    // Introspection
    // ====================================================================
    int SixDofMaskProcessor::getNumDetectedSources() const
    {
        std::lock_guard<std::mutex> lock(m_sourcesMutex);
        return (int) m_detectedSources.size();
    }

    int SixDofMaskProcessor::getDetectedSourcePositions(float* outPositions, int maxSources) const
    {
        std::lock_guard<std::mutex> lock(m_sourcesMutex);
        const int n = std::min(maxSources, (int) m_detectedSources.size());
        for (int i = 0; i < n; ++i)
        {
            outPositions[i * 3 + 0] = m_detectedSources[(size_t) i].x;
            outPositions[i * 3 + 1] = m_detectedSources[(size_t) i].y;
            outPositions[i * 3 + 2] = m_detectedSources[(size_t) i].z;
        }
        return n;
    }

    // ====================================================================
    // Background thread — localization
    // ====================================================================
    void SixDofMaskProcessor::backgroundThreadLoop()
    {
        juce::Thread::setCurrentThreadName("AT_SixDofMask_Localization");

        while (!m_shouldStop.load(std::memory_order_relaxed))
        {
            std::vector<std::vector<float>> window;
            {
                std::unique_lock<std::mutex> lock(m_pendingWindowMutex);
                m_pendingWindowCv.wait(lock, [this]
                {
                    return m_pendingWindowReady || m_shouldStop.load(std::memory_order_relaxed);
                });
                if (m_shouldStop.load(std::memory_order_relaxed))
                    break;
                window = m_pendingWindow; // copy out, release lock quickly
                m_pendingWindowReady = false;
            }

            runLocalizationOnWindow(window);
        }
    }

    void SixDofMaskProcessor::runLocalizationOnWindow(const std::vector<std::vector<float>>& window)
    {
        if (window.empty() || window[0].empty())
            return;

        const int numSamples = (int) window[0].size();
        const float* refPtr = window[(size_t) REF_CHANNEL].data();

        // Skip near-silent windows outright (matches the Python prototype's
        // "bloc quasi silencieux, ignoré" guard) — avoids feeding GCC-PHAT
        // pure noise, which is exactly the case that produced wildly
        // divergent positions on the first real-corpus test (see 6dof-nav
        // skill, "diag_localization.py" findings).
        float peak = 0.0f;
        float refPeak = 0.0f;
        for (int ch = 0; ch < m_numChannels; ++ch)
            for (float v : window[(size_t) ch])
                peak = std::max(peak, std::abs(v));
        for (int i = 0; i < numSamples; ++i)
            refPeak = std::max(refPeak, std::abs(refPtr[i]));

#if AT_SIXDOF_DEBUG_LOG
        std::cout << "[6DOF][localize] window peak=" << peak
                   << "  ref_ch" << REF_CHANNEL << "_peak=" << refPeak;
        if (refPeak < 1e-6f && peak >= 1e-6f)
            std::cout << "  <<< REF CHANNEL LOOKS DEAD (silent) WHILE OTHERS AREN'T";
        std::cout << "\n";
#endif

        if (peak < 1e-6f)
        {
#if AT_SIXDOF_DEBUG_LOG
            std::cout << "[6DOF][localize] window SKIPPED (near-silent)\n";
#endif
            return;
        }

        std::vector<double> delaysSeconds((size_t) m_numChannels, 0.0);
        std::vector<char>   validMask((size_t) m_numChannels, 0);
        delaysSeconds[REF_CHANNEL] = 0.0;
        validMask[REF_CHANNEL] = 1;

        for (int ch = 0; ch < m_numChannels; ++ch)
        {
            if (ch == REF_CHANNEL) continue;
            double delaySamples = 0.0;
            if (gccPhatDelaySamples(refPtr, window[(size_t) ch].data(), numSamples, delaySamples))
            {
                delaysSeconds[(size_t) ch] = delaySamples / m_sampleRate;
                validMask[(size_t) ch] = 1;
            }
        }

#if AT_SIXDOF_DEBUG_LOG
        int numValid = 0;
        double minDelay = 1e300, maxDelay = -1e300;
        for (int ch = 0; ch < m_numChannels; ++ch)
        {
            if (!validMask[(size_t) ch]) continue;
            numValid++;
            minDelay = std::min(minDelay, delaysSeconds[(size_t) ch]);
            maxDelay = std::max(maxDelay, delaysSeconds[(size_t) ch]);
        }
        std::cout << "[6DOF][localize] GCC-PHAT valid channels: " << numValid << "/" << m_numChannels
                   << "  delay range=[" << minDelay << ", " << maxDelay << "] s"
                   << "  (= [" << minDelay * SPEED_OF_SOUND << ", " << maxDelay * SPEED_OF_SOUND << "] m)\n";
#endif

        SixDofSourcePosition estimate;
        double fitResidual = 0.0;
        if (!sphericalInterpolation(delaysSeconds, validMask, estimate, fitResidual))
        {
#if AT_SIXDOF_DEBUG_LOG
            std::cout << "[6DOF][localize] spherical interpolation FAILED (degenerate geometry "
                         "or <4 valid channels)\n";
#endif
            return;
        }

#if AT_SIXDOF_DEBUG_LOG
        std::cout << "[6DOF][localize] raw estimate = (" << estimate.x << ", "
                   << estimate.y << ", " << estimate.z << ")  fit_residual=" << fitResidual << "\n";
#endif

        // Fit-quality filter: a window where different mics locked onto
        // different simultaneously-active sources produces data that is
        // mathematically inconsistent with ANY single point source — the
        // solve above still "succeeds" (it's just linear algebra) but the
        // result is physically meaningless. See MAX_FIT_RESIDUAL's comment
        // for the empirical validation (clean ~0 vs mixed ~4+, huge margin).
        if (fitResidual > MAX_FIT_RESIDUAL)
        {
#if AT_SIXDOF_DEBUG_LOG
            std::cout << "[6DOF][localize] REJECTED: fit_residual=" << fitResidual
                       << " > max=" << MAX_FIT_RESIDUAL
                       << " (likely mixed/contaminated by multiple simultaneous sources)\n";
#endif
            return;
        }

        // Plausibility filter (see prepare(): m_maxPlausibleDist).
        const float dx = estimate.x - m_arrayCentroid[0];
        const float dy = estimate.y - m_arrayCentroid[1];
        const float dz = estimate.z - m_arrayCentroid[2];
        const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (dist > m_maxPlausibleDist)
        {
#if AT_SIXDOF_DEBUG_LOG
            std::cout << "[6DOF][localize] REJECTED: dist_from_centroid=" << dist
                       << " > max_plausible=" << m_maxPlausibleDist << "\n";
#endif
            return; // silently drop — same rationale as the Python prototype
        }

        pushEstimateAndUpdateModes(estimate);
    }

    bool SixDofMaskProcessor::gccPhatDelaySamples(const float* ref, const float* ch, int numSamples,
                                                    double& outDelaySamples)
    {
        // Generalized Cross-Correlation with PHAT weighting (Knapp & Carter
        // 1976) — combines all frequencies at once, so (unlike a single-
        // frequency phase read) it does not suffer the spatial-aliasing
        // ambiguity a wide-aperture array would otherwise hit. See 6dof-nav
        // skill for the full rationale vs. a naive phase-difference read.
        if (numSamples <= 0 || numSamples * 2 > m_fftSize)
            return false;

        std::fill(m_fftBufA.begin(), m_fftBufA.end(), 0.0f);
        std::fill(m_fftBufB.begin(), m_fftBufB.end(), 0.0f);
        for (int i = 0; i < numSamples; ++i)
        {
            m_fftBufA[(size_t) i * 2] = ref[i];
            m_fftBufB[(size_t) i * 2] = ch[i];
        }

        m_fft->perform(reinterpret_cast<juce::dsp::Complex<float>*>(m_fftBufA.data()),
                        reinterpret_cast<juce::dsp::Complex<float>*>(m_fftBufA.data()), false);
        m_fft->perform(reinterpret_cast<juce::dsp::Complex<float>*>(m_fftBufB.data()),
                        reinterpret_cast<juce::dsp::Complex<float>*>(m_fftBufB.data()), false);

        // Cross-power spectrum R = A * conj(B), PHAT-normalized (magnitude
        // divided out), stored back into m_fftBufA.
        for (int i = 0; i < m_fftSize; ++i)
        {
            const float ar = m_fftBufA[(size_t) i * 2],     ai = m_fftBufA[(size_t) i * 2 + 1];
            const float br = m_fftBufB[(size_t) i * 2],     bi = m_fftBufB[(size_t) i * 2 + 1];
            // A * conj(B)
            const float rr = ar * br + ai * bi;
            const float ri = ai * br - ar * bi;
            const float mag = std::sqrt(rr * rr + ri * ri) + 1e-12f;
            m_fftBufA[(size_t) i * 2]     = rr / mag;
            m_fftBufA[(size_t) i * 2 + 1] = ri / mag;
        }

        m_fft->perform(reinterpret_cast<juce::dsp::Complex<float>*>(m_fftBufA.data()),
                        reinterpret_cast<juce::dsp::Complex<float>*>(m_fftBufA.data()), true);

        // Search only within the physically plausible delay range (matches
        // MAX_TDOA_SECONDS in the Python prototype) — bounds the peak search
        // and rejects correlation noise outside any sensible array extent.
        const int maxShift = std::min(m_fftSize / 2 - 1,
                                       (int) std::ceil(m_maxDelaySeconds * m_sampleRate));

        int bestIndex = 0;
        float bestVal = -1.0f;
        // Index 0..maxShift -> positive delays 0..maxShift
        for (int i = 0; i <= maxShift; ++i)
        {
            const float mag = std::abs(m_fftBufA[(size_t) i * 2]);
            if (mag > bestVal) { bestVal = mag; bestIndex = i; }
        }
        // Index fftSize-maxShift..fftSize-1 -> negative delays -maxShift..-1
        for (int i = m_fftSize - maxShift; i < m_fftSize; ++i)
        {
            const float mag = std::abs(m_fftBufA[(size_t) i * 2]);
            if (mag > bestVal) { bestVal = mag; bestIndex = i - m_fftSize; }
        }

        outDelaySamples = (double) bestIndex;
        return true;
    }

    bool SixDofMaskProcessor::sphericalInterpolation(const std::vector<double>& delaysSeconds,
                                                        const std::vector<char>& validMask,
                                                        SixDofSourcePosition& outPos,
                                                        double& outResidual) const
    {
        // Smith & Abel (1987) spherical interpolation: closed-form TDOA
        // multilateration via a single 4x4 linear solve (unknowns:
        // Sx, Sy, Sz, R_ref) — no iterative search, no grid. Direct port of
        // spherical_interpolation() in apply_mask.py.
        const auto& ref = m_micPositions[(size_t) REF_CHANNEL];
        const double refX = ref[0], refY = ref[1], refZ = ref[2];
        const double refDotRef = refX * refX + refY * refY + refZ * refZ;

        double AtA[4][4] = {{0}};
        double Atb[4]    = {0};
        int usedCount = 0;

        // Kept for the post-solve residual check (see below) — how well the
        // solved position actually satisfies each individual TDOA equation.
        std::vector<std::array<double, 4>> rows;
        std::vector<double> bs;
        rows.reserve((size_t) m_numChannels);
        bs.reserve((size_t) m_numChannels);

        for (int i = 0; i < m_numChannels; ++i)
        {
            if (i == REF_CHANNEL || !validMask[(size_t) i])
                continue;

            const auto& mi = m_micPositions[(size_t) i];
            const double d = SPEED_OF_SOUND * delaysSeconds[(size_t) i];

            const double row[4] = {
                -2.0 * (mi[0] - refX),
                -2.0 * (mi[1] - refY),
                -2.0 * (mi[2] - refZ),
                -2.0 * d
            };
            const double miDotMi = (double) mi[0] * mi[0] + (double) mi[1] * mi[1] + (double) mi[2] * mi[2];
            const double b = d * d - miDotMi + refDotRef;

            for (int r = 0; r < 4; ++r)
            {
                for (int c = 0; c < 4; ++c)
                    AtA[r][c] += row[r] * row[c];
                Atb[r] += row[r] * b;
            }
            rows.push_back({ row[0], row[1], row[2], row[3] });
            bs.push_back(b);
            ++usedCount;
        }

        if (usedCount < 4)
            return false; // under-determined, geometry too sparse

        // Tikhonov (ridge) regularization: for a PLANAR mic array (e.g. a
        // flat circle, all y=0 — this test app's default config, and close
        // to true even for the real equator ring), the column corresponding
        // to the out-of-plane axis is exactly (or nearly) zero for every
        // row, making AtA exactly singular on that axis — solve4x4's
        // Gaussian elimination then fails outright on the zero pivot.
        // Python's np.linalg.lstsq handles this gracefully via SVD (returns
        // the minimum-norm solution, i.e. the under-constrained axis solves
        // to ~0) — this small diagonal epsilon reproduces that behaviour
        // for our hand-rolled 4x4 solve without needing a full SVD.
        // Confirmed via console diagnostic logs: 100% "degenerate geometry"
        // failures on a flat-circle test config before this fix.
        constexpr double REG_EPS = 1e-6;
        for (int i = 0; i < 4; ++i)
            AtA[i][i] += REG_EPS;

#if AT_SIXDOF_DEBUG_LOG
        std::cout << "[6DOF][solve] usedCount=" << usedCount
                   << "  AtA diag=(" << AtA[0][0] << ", " << AtA[1][1] << ", "
                   << AtA[2][2] << ", " << AtA[3][3] << ")\n";
#endif

        double x[4] = {0};
        if (!solve4x4(AtA, Atb, x))
            return false;

        // Fit-quality residual: how well the solved (x,y,z,R_ref) actually
        // satisfies each individual TDOA equation. A genuine single point
        // source gives a residual of essentially zero (validated on a clean
        // synthetic test: ~1e-10). Delays contaminated by a mix of two
        // simultaneously-active sources (different mic pairs each locking
        // onto whichever source dominates their own cross-correlation) are
        // mathematically INCONSISTENT with any single point source — the
        // least-squares solve still returns "an answer", but a physically
        // meaningless one (often near the array center, sometimes with a
        // negative/nonsensical R_ref) — validated empirically: a mixed-
        // source test gave a residual ~7 orders of magnitude larger than
        // the clean case (0.000 vs ~4.1). This residual is therefore a far
        // more reliable rejection criterion than trying to detect "which
        // mic locked onto which source" directly.
        double sumSq = 0.0;
        for (size_t i = 0; i < rows.size(); ++i)
        {
            const auto& row = rows[i];
            double predicted = row[0] * x[0] + row[1] * x[1] + row[2] * x[2] + row[3] * x[3];
            double diff = predicted - bs[i];
            sumSq += diff * diff;
        }
        outResidual = std::sqrt(sumSq / (double) rows.size());

        outPos.x = (float) x[0];
        outPos.y = (float) x[1];
        outPos.z = (float) x[2];
#if AT_SIXDOF_DEBUG_LOG
        std::cout << "[6DOF][solve] R_ref (solved) = " << x[3]
                   << "  fit_residual_rms = " << outResidual << "\n";
#endif
        return true;
    }

    void SixDofMaskProcessor::pushEstimateAndUpdateModes(const SixDofSourcePosition& estimate)
    {
        m_estimateHistory[(size_t) m_historyWritePos] = estimate;
        m_historyWritePos = (m_historyWritePos + 1) % HISTORY_SIZE;
        m_historyCount = std::min(m_historyCount + 1, HISTORY_SIZE);

        // Mode search (histogram on a grid_res-rounded position) over the
        // rolling history — direct port of the mode-detection logic in
        // apply_mask.py's localize_sources(): genuine dominant sources
        // repeat almost exactly from window to window, while windows with
        // several simultaneously-active sources produce scattered
        // "compromise" positions that (almost) never repeat. Counting
        // occurrences therefore separates signal from that specific kind of
        // noise far more robustly than proximity-based clustering (k-means)
        // — validated in the Python prototype, see 6dof-nav skill.
        const float gridRes = m_gridRes.load(std::memory_order_relaxed);
        const int   minCount = m_minBlockCount.load(std::memory_order_relaxed);

        struct Bucket { std::array<float, 3> roundedPos; int count; };
        std::vector<Bucket> buckets;
        buckets.reserve((size_t) m_historyCount);

        for (int i = 0; i < m_historyCount; ++i)
        {
            const auto& p = m_estimateHistory[(size_t) i];
            std::array<float, 3> rounded = {
                std::round(p.x / gridRes) * gridRes,
                std::round(p.y / gridRes) * gridRes,
                std::round(p.z / gridRes) * gridRes
            };

            bool found = false;
            for (auto& bucket : buckets)
            {
                if (bucket.roundedPos == rounded) { ++bucket.count; found = true; break; }
            }
            if (!found)
                buckets.push_back({ rounded, 1 });
        }

        std::sort(buckets.begin(), buckets.end(),
                  [](const Bucket& a, const Bucket& b) { return a.count > b.count; });

#if AT_SIXDOF_DEBUG_LOG
        std::cout << "[6DOF][mode] history=" << m_historyCount << "/" << HISTORY_SIZE
                   << "  grid_res=" << gridRes << "  min_block_count=" << minCount
                   << "  distinct_buckets=" << buckets.size() << "\n";
        {
            int shown = 0;
            for (const auto& bucket : buckets)
            {
                std::cout << "[6DOF][mode]   (" << bucket.roundedPos[0] << ", "
                           << bucket.roundedPos[1] << ", " << bucket.roundedPos[2]
                           << ")  x" << bucket.count
                           << (bucket.count >= minCount ? "  <- ACCEPTED" : "") << "\n";
                if (++shown >= 10) { std::cout << "[6DOF][mode]   ...\n"; break; }
            }
        }
#endif

        std::vector<SixDofSourcePosition> newSources;
        for (const auto& bucket : buckets)
        {
            if (bucket.count < minCount)
                break; // sorted descending: nothing further qualifies either
            newSources.push_back({ bucket.roundedPos[0], bucket.roundedPos[1], bucket.roundedPos[2] });
            if ((int) newSources.size() >= MAX_SOURCES)
                break;
        }

        // ---- Per-source temporal hysteresis -------------------------------
        // Match this cycle's accepted positions (newSources) against
        // candidates already being tracked; promote/demote with a grace
        // period on both ends (see TrackedCandidate's doc comment) instead
        // of blindly replacing m_detectedSources with newSources every
        // cycle. This is what actually fixes the single-cycle dropout —
        // the global Valid/Stale/Bypass machinery below still exists
        // separately, for the case where EVERY tracked source is lost.
        std::vector<bool> matched(newSources.size(), false);
        for (auto& cand : m_trackedCandidates)
        {
            int bestIdx = -1;
            float bestDist = SOURCE_MATCH_DIST;
            for (size_t j = 0; j < newSources.size(); ++j)
            {
                if (matched[j]) continue;
                const float dx = newSources[j].x - cand.pos.x;
                const float dy = newSources[j].y - cand.pos.y;
                const float dz = newSources[j].z - cand.pos.z;
                const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (dist < bestDist) { bestDist = dist; bestIdx = (int) j; }
            }
            if (bestIdx >= 0)
            {
                cand.pos = newSources[(size_t) bestIdx];
                cand.presentStreak++;
                cand.absentStreak = 0;
                matched[(size_t) bestIdx] = true;
                if (!cand.confirmed && cand.presentStreak >= SOURCE_CONFIRM_CYCLES)
                    cand.confirmed = true;
            }
            else
            {
                cand.presentStreak = 0;
                cand.absentStreak++;
            }
        }
        // Drop candidates gone too long: confirmed ones get SOURCE_GRACE_CYCLES
        // of slack; never-yet-confirmed provisional ones are dropped on the
        // very first miss (no point tracking a phantom that didn't even
        // survive to confirmation).
        m_trackedCandidates.erase(
            std::remove_if(m_trackedCandidates.begin(), m_trackedCandidates.end(),
                [](const TrackedCandidate& c)
                {
                    return c.confirmed ? (c.absentStreak > SOURCE_GRACE_CYCLES)
                                        : (c.absentStreak > 0);
                }),
            m_trackedCandidates.end());
        // New, unmatched candidates start provisional.
        for (size_t j = 0; j < newSources.size(); ++j)
        {
            if (matched[j]) continue;
            if ((int) m_trackedCandidates.size() >= MAX_SOURCES) break;
            TrackedCandidate c;
            c.pos = newSources[j];
            c.presentStreak = 1;
            c.confirmed = (SOURCE_CONFIRM_CYCLES <= 1);
            m_trackedCandidates.push_back(c);
        }

        std::vector<SixDofSourcePosition> confirmedSources;
        for (const auto& cand : m_trackedCandidates)
            if (cand.confirmed)
                confirmedSources.push_back(cand.pos);

#if AT_SIXDOF_DEBUG_LOG
        std::cout << "[6DOF][track] tracked=" << m_trackedCandidates.size()
                   << "  confirmed=" << confirmedSources.size()
                   << "  (raw this-cycle accepted=" << newSources.size() << ")\n";
#endif

        // ---- Detection failure protection state machine -------------------
        // Now driven by confirmedSources (post-hysteresis), not the raw
        // per-cycle newSources — a single missed cycle for one source no
        // longer touches this machinery at all, since the OTHER confirmed
        // sources (if any) keep confirmedSources non-empty. MAX_STALE_CYCLES
        // only kicks in once every tracked source has exhausted its own
        // grace period too.
        if (!confirmedSources.empty())
        {
            std::lock_guard<std::mutex> lock(m_sourcesMutex);
            m_detectedSources = std::move(confirmedSources);
            m_state.store(State::Valid, std::memory_order_relaxed);
            m_staleCycleCount = 0;
#if AT_SIXDOF_DEBUG_LOG
            std::cout << "[6DOF][state] -> Valid (" << m_detectedSources.size() << " source(s))\n";
#endif
        }
        else
        {
            ++m_staleCycleCount;
            if (m_staleCycleCount > MAX_STALE_CYCLES)
            {
                std::lock_guard<std::mutex> lock(m_sourcesMutex);
                m_detectedSources.clear();
                m_state.store(State::Bypass, std::memory_order_relaxed);
#if AT_SIXDOF_DEBUG_LOG
                std::cout << "[6DOF][state] -> Bypass (stale_cycles=" << m_staleCycleCount
                           << " > " << MAX_STALE_CYCLES << ", giving up on last known sources)\n";
#endif
            }
            else
            {
                m_state.store(State::Stale, std::memory_order_relaxed);
                // m_detectedSources deliberately left untouched — keep
                // serving the last known-good positions during the grace
                // period.
#if AT_SIXDOF_DEBUG_LOG
                std::cout << "[6DOF][state] -> Stale (stale_cycles=" << m_staleCycleCount
                           << "/" << MAX_STALE_CYCLES << ", keeping last known sources)\n";
#endif
            }
        }
    }
}
