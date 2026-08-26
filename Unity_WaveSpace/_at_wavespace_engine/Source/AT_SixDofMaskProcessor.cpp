/**
 * @file AT_SixDofMaskProcessor.cpp
 * @author Antoine Gonot / Claude
 * @date 2026
 */

#include "AT_SixDofMaskProcessor.h"
#include <cmath>
#include <complex>
#include <algorithm>
#include <numeric>

// ── TEMPORARY diagnostic logging — console app only, plain std::cout (no
// Unity/LOG() dependency). Flip to 0 to silence once diagnosis is done;
// left in place (not deleted) so it can be re-enabled without rewriting.
#define AT_SIXDOF_DEBUG_LOG 0

namespace AT
{
    // ====================================================================
    // In-house real-symmetric Jacobi eigenvalue solver — the entire
    // replacement for np.linalg.eigh() used by the Python prototypes. See
    // computeSignalSubspace()'s doc comment (header) for why a REAL
    // symmetric solver is enough to diagonalize the COMPLEX Hermitian
    // covariance matrix MUSIC needs.
    // ====================================================================
    namespace
    {
        /**
         * @brief Classical cyclic Jacobi eigenvalue algorithm.
         * @param A  n x n real symmetric matrix, row-major. Diagonalized in
         *           place: after return, A's diagonal holds the eigenvalues
         *           (unsorted) and its off-diagonal entries are ~0.
         * @param n  Matrix dimension.
         * @param V  Filled with the eigenvectors as columns (V[row*n+col]),
         *           same order as A's diagonal.
         *
         * No third-party dependency, no external library — a handful of
         * classical Givens-style rotations repeated until the off-diagonal
         * energy is negligible. Adequate for the background-thread,
         * periodic-update cost budget this feature targets (see the
         * 6dof-nav skill's real-time feasibility notes) — not intended to
         * compete with a tuned LAPACK routine on very large matrices.
         */
        void jacobiEigenSymmetric(std::vector<double>& A, int n, std::vector<double>& V, int maxSweeps = 60)
        {
            V.assign((size_t) n * (size_t) n, 0.0);
            for (int i = 0; i < n; ++i)
                V[(size_t) i * n + i] = 1.0;
            if (n <= 1)
                return;

            for (int sweep = 0; sweep < maxSweeps; ++sweep)
            {
                double offDiagSum = 0.0;
                for (int p = 0; p < n - 1; ++p)
                    for (int q = p + 1; q < n; ++q)
                        offDiagSum += A[(size_t) p * n + q] * A[(size_t) p * n + q];
                if (offDiagSum < 1e-18)
                    break; // converged

                for (int p = 0; p < n - 1; ++p)
                {
                    for (int q = p + 1; q < n; ++q)
                    {
                        const double apq = A[(size_t) p * n + q];
                        if (std::abs(apq) < 1e-300)
                            continue;

                        const double app = A[(size_t) p * n + p];
                        const double aqq = A[(size_t) q * n + q];
                        const double theta = (aqq - app) / (2.0 * apq);
                        const double t = (theta >= 0.0 ? 1.0 : -1.0)
                                        / (std::abs(theta) + std::sqrt(theta * theta + 1.0));
                        const double c = 1.0 / std::sqrt(t * t + 1.0);
                        const double s = t * c;

                        A[(size_t) p * n + p] = app - t * apq;
                        A[(size_t) q * n + q] = aqq + t * apq;
                        A[(size_t) p * n + q] = 0.0;
                        A[(size_t) q * n + p] = 0.0;

                        for (int i = 0; i < n; ++i)
                        {
                            if (i == p || i == q) continue;
                            const double aip = A[(size_t) i * n + p];
                            const double aiq = A[(size_t) i * n + q];
                            const double newip = c * aip - s * aiq;
                            const double newiq = s * aip + c * aiq;
                            A[(size_t) i * n + p] = newip; A[(size_t) p * n + i] = newip;
                            A[(size_t) i * n + q] = newiq; A[(size_t) q * n + i] = newiq;
                        }
                        for (int i = 0; i < n; ++i)
                        {
                            const double vip = V[(size_t) i * n + p];
                            const double viq = V[(size_t) i * n + q];
                            V[(size_t) i * n + p] = c * vip - s * viq;
                            V[(size_t) i * n + q] = s * vip + c * viq;
                        }
                    }
                }
            }
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
        m_arrayExtent = std::max(0.5f, maxExtent); // search grid radius (buildSearchGrid)
        m_maxPlausibleDist = std::max(1.0f, 10.0f * maxExtent);

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

        // FFT sized for a SINGLE STFT analysis frame (fixed, small — unlike
        // the previous GCC-PHAT implementation, which sized its FFT for the
        // whole analysis window). Reused once per hop per channel.
        m_fftOrder = 0;
        while ((1 << m_fftOrder) < STFT_FRAME_SIZE)
            ++m_fftOrder;
        m_fft = std::make_unique<juce::dsp::FFT>(m_fftOrder);
        m_fftScratch.assign((size_t) STFT_FRAME_SIZE * 2, 0.0f);

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

    void SixDofMaskProcessor::setMaxSources(int maxSources)
    {
        m_maxSources.store(juce::jlimit(1, MAX_SOURCES, maxSources), std::memory_order_relaxed);
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
        // by the audio thread (see pushAnalysisBlock()) — never mutated mid-window.
    }

    void SixDofMaskProcessor::setSearchGridResolution(float searchGridResolution)
    {
        m_searchGridResolution.store(std::max(0.01f, searchGridResolution), std::memory_order_relaxed);
    }

    void SixDofMaskProcessor::setMaxBins(int maxBins)
    {
        m_maxBins.store(juce::jlimit(1, 64, maxBins), std::memory_order_relaxed);
    }

    void SixDofMaskProcessor::setBandHzMin(float bandHzMin)
    {
        m_bandHzMin.store(std::max(0.0f, bandHzMin), std::memory_order_relaxed);
    }

    void SixDofMaskProcessor::setBandHzMax(float bandHzMax)
    {
        m_bandHzMax.store(std::max(1.0f, bandHzMax), std::memory_order_relaxed);
    }

    void SixDofMaskProcessor::setYRangeMin(float yRangeMin)
    {
        m_yRangeMin.store(yRangeMin, std::memory_order_relaxed);
    }

    void SixDofMaskProcessor::setYRangeMax(float yRangeMax)
    {
        m_yRangeMax.store(yRangeMax, std::memory_order_relaxed);
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
        // Direct port of mask_weights() in apply_mask_MUSIC.py: a microphone
        // is valid (weight -> 1) iff the source lies between the listener
        // and that microphone; invalid (weight -> 0) otherwise, with a
        // MASK_TRANSITION-metre continuous ramp instead of a hard cutoff
        // (avoids zipper artifacts as the listener moves).
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

    // ====================================================================
    // MUSIC localization
    // ====================================================================
    std::vector<SixDofMaskProcessor::GridPoint>
    SixDofMaskProcessor::buildSearchGrid(float resolution, float yMin, float yMax) const
    {
        // Direct port of build_grid() in apply_mask_MUSIC.py: a disc of
        // radius equal to the array's own extent (X/Z plane), swept over
        // [yMin, yMax] in Y at 2x the horizontal resolution (less vertical
        // precision needed in practice — matches the Python default).
        std::vector<GridPoint> grid;
        const float radius = m_arrayExtent;
        if (resolution <= 0.0f || radius <= 0.0f)
            return grid;

        const float radiusLimit = radius * 0.98f;
        const float yStep = std::max(resolution * 2.0f, 0.01f);
        for (float x = -radius; x <= radius + 1e-4f; x += resolution)
        {
            for (float z = -radius; z <= radius + 1e-4f; z += resolution)
            {
                if (std::sqrt(x * x + z * z) >= radiusLimit)
                    continue;
                for (float y = yMin; y <= yMax + 1e-4f; y += yStep)
                    grid.push_back({ x, y, z });
            }
        }
        return grid;
    }

    void SixDofMaskProcessor::steeringVector(const GridPoint& pos, float freqHz,
                                               std::vector<std::complex<float>>& outA) const
    {
        // a_i(p,f) = (1/r_i) * exp(-j*2*pi*f*r_i/c), normalized to unit norm
        // — same physical model (propagation delay + 1/r decay) as
        // simulate_capture.py / apply_mask_MUSIC.py's steering_vector().
        outA.resize((size_t) m_numChannels);
        float normSq = 0.0f;
        for (int i = 0; i < m_numChannels; ++i)
        {
            const auto& mic = m_micPositions[(size_t) i];
            const float dx = mic[0] - pos.x, dy = mic[1] - pos.y, dz = mic[2] - pos.z;
            const float dist = std::max(std::sqrt(dx * dx + dy * dy + dz * dz), MIN_DIST);
            const float phase = -2.0f * juce::MathConstants<float>::pi * freqHz * dist / SPEED_OF_SOUND;
            const std::complex<float> val = std::polar(1.0f / dist, phase);
            outA[(size_t) i] = val;
            normSq += std::norm(val);
        }
        const float norm = std::sqrt(std::max(normSq, 1e-20f));
        for (auto& v : outA)
            v /= norm;
    }

    void SixDofMaskProcessor::computeSignalSubspace(const std::vector<std::complex<float>>& K, int numSources,
                                                       std::vector<std::vector<std::complex<float>>>& outSignalSubspace) const
    {
        outSignalSubspace.clear();
        const int n = m_numChannels;
        if (n < 2)
            return;
        numSources = juce::jlimit(1, n - 1, numSources);

        // Real 2n x 2n block embedding of the complex Hermitian K — see the
        // header's computeSignalSubspace() doc comment for the derivation.
        const int n2 = 2 * n;
        std::vector<double> M((size_t) n2 * (size_t) n2, 0.0);
        for (int r = 0; r < n; ++r)
        {
            for (int c = 0; c < n; ++c)
            {
                const std::complex<float> k = K[(size_t) r * n + c];
                const double re = (double) k.real();
                const double im = (double) k.imag();
                M[(size_t) r * n2 + c]             = re;   // top-left:     Kre
                M[(size_t) r * n2 + (c + n)]       = -im;  // top-right:   -Kim
                M[(size_t) (r + n) * n2 + c]       = im;   // bottom-left:  Kim
                M[(size_t) (r + n) * n2 + (c + n)] = re;   // bottom-right: Kre
            }
        }

        std::vector<double> V;
        jacobiEigenSymmetric(M, n2, V);

        std::vector<int> order(n2);
        std::iota(order.begin(), order.end(), 0);
        std::sort(order.begin(), order.end(),
                  [&](int a, int b) { return M[(size_t) a * n2 + a] > M[(size_t) b * n2 + b]; });

        // Every real eigenvalue of M is doubled (one complex eigenvalue of K
        // -> two real eigenvalues of M) — walk the descending list, and once
        // an index is used, also mark its numerically-closest still-unused
        // neighbour as consumed (its degenerate partner), so it isn't picked
        // again as a spurious extra "source" a few steps later.
        std::vector<bool> used(n2, false);
        for (int idx = 0; idx < n2 && (int) outSignalSubspace.size() < numSources; ++idx)
        {
            const int i = order[idx];
            if (used[i]) continue;
            used[i] = true;

            const double lambda = M[(size_t) i * n2 + i];
            for (int idx2 = idx + 1; idx2 < n2; ++idx2)
            {
                const int j = order[idx2];
                if (used[j]) continue;
                if (std::abs(M[(size_t) j * n2 + j] - lambda) < 1e-6 * (std::abs(lambda) + 1e-12))
                {
                    used[j] = true;
                    break;
                }
            }

            std::vector<std::complex<float>> w((size_t) n);
            for (int r = 0; r < n; ++r)
                w[(size_t) r] = std::complex<float>((float) V[(size_t) r * n2 + i],
                                                       (float) V[(size_t) (r + n) * n2 + i]);
            outSignalSubspace.push_back(std::move(w));
        }
    }

    void SixDofMaskProcessor::runLocalizationOnWindow(const std::vector<std::vector<float>>& window)
    {
        if (window.empty() || window[0].empty())
            return;

        const int numSamples = (int) window[0].size();

        // Skip near-silent windows outright (matches the Python prototype's
        // "bloc quasi silencieux, ignoré" guard).
        float peak = 0.0f;
        for (int ch = 0; ch < m_numChannels; ++ch)
            for (float v : window[(size_t) ch])
                peak = std::max(peak, std::abs(v));

#if AT_SIXDOF_DEBUG_LOG
        std::cout << "[6DOF][localize] window peak=" << peak << "  samples=" << numSamples;
#endif
        if (peak < 1e-6f)
        {
#if AT_SIXDOF_DEBUG_LOG
            std::cout << "  SKIPPED (near-silent)\n";
#endif
            return;
        }
        if (numSamples < STFT_FRAME_SIZE)
        {
#if AT_SIXDOF_DEBUG_LOG
            std::cout << "  SKIPPED (window shorter than one STFT frame, "
                       << STFT_FRAME_SIZE << " samples needed)\n";
#endif
            return;
        }
#if AT_SIXDOF_DEBUG_LOG
        std::cout << "\n";
#endif

        const int maxSrc      = juce::jlimit(1, MAX_SOURCES, m_maxSources.load(std::memory_order_relaxed));
        const int maxBins     = m_maxBins.load(std::memory_order_relaxed);
        const float bandMin   = m_bandHzMin.load(std::memory_order_relaxed);
        const float bandMax   = m_bandHzMax.load(std::memory_order_relaxed);
        const float gridRes   = m_searchGridResolution.load(std::memory_order_relaxed);
        const float yMin      = m_yRangeMin.load(std::memory_order_relaxed);
        const float yMax      = m_yRangeMax.load(std::memory_order_relaxed);

        // ---- Retained frequency bins, linearly spread across [bandMin, bandMax] ----
        int binLo = juce::jlimit(1, STFT_FRAME_SIZE / 2, (int) std::round(bandMin * STFT_FRAME_SIZE / m_sampleRate));
        int binHi = juce::jlimit(1, STFT_FRAME_SIZE / 2, (int) std::round(bandMax * STFT_FRAME_SIZE / m_sampleRate));
        if (binHi < binLo) std::swap(binLo, binHi);
        std::vector<int> binIndices;
        binIndices.reserve((size_t) maxBins);
        for (int i = 0; i < maxBins; ++i)
        {
            const float t = (maxBins == 1) ? 0.0f : (float) i / (float) (maxBins - 1);
            binIndices.push_back(binLo + (int) std::round(t * (float) (binHi - binLo)));
        }

        // ---- STFT: one small FFT per channel per hop, keep only the
        //      retained bins (avoids holding full spectra for every frame) ----
        const int numFrames = (numSamples - STFT_FRAME_SIZE) / STFT_HOP_SIZE + 1;
        if (numFrames < 1)
            return;

        std::vector<std::vector<std::vector<std::complex<float>>>> snapshots(
            binIndices.size(),
            std::vector<std::vector<std::complex<float>>>((size_t) numFrames,
                std::vector<std::complex<float>>((size_t) m_numChannels)));

        for (int ch = 0; ch < m_numChannels; ++ch)
        {
            const float* chData = window[(size_t) ch].data();
            for (int f = 0; f < numFrames; ++f)
            {
                const int offset = f * STFT_HOP_SIZE;
                std::fill(m_fftScratch.begin(), m_fftScratch.end(), 0.0f);
                for (int n = 0; n < STFT_FRAME_SIZE; ++n)
                {
                    // Hann window, matches scipy.signal.stft's default.
                    const float w = 0.5f - 0.5f * std::cos(2.0f * juce::MathConstants<float>::pi * n
                                                             / (float) (STFT_FRAME_SIZE - 1));
                    m_fftScratch[(size_t) n * 2] = chData[offset + n] * w;
                }
                m_fft->perform(reinterpret_cast<juce::dsp::Complex<float>*>(m_fftScratch.data()),
                                reinterpret_cast<juce::dsp::Complex<float>*>(m_fftScratch.data()), false);

                for (size_t bi = 0; bi < binIndices.size(); ++bi)
                {
                    const int b = binIndices[bi];
                    snapshots[bi][(size_t) f][(size_t) ch] =
                        std::complex<float>(m_fftScratch[(size_t) b * 2], m_fftScratch[(size_t) b * 2 + 1]);
                }
            }
        }

        // ---- Search grid + pseudo-spectrum, accumulated across bands
        //      ("incoherent combination") ----
        std::vector<GridPoint> grid = buildSearchGrid(gridRes, yMin, yMax);
        if (grid.empty())
        {
#if AT_SIXDOF_DEBUG_LOG
            std::cout << "[6DOF][localize] SKIPPED (empty search grid — check search grid "
                         "resolution / array extent)\n";
#endif
            return;
        }
        std::vector<double> spectrum(grid.size(), 0.0);

        std::vector<std::complex<float>> K((size_t) m_numChannels * (size_t) m_numChannels);
        std::vector<std::vector<std::complex<float>>> signalSubspace;
        std::vector<std::complex<float>> a;

        for (size_t bi = 0; bi < binIndices.size(); ++bi)
        {
            const float freqHz = (float) binIndices[bi] * (float) m_sampleRate / (float) STFT_FRAME_SIZE;

            // K = (1/T) * sum_t x_t x_t^H — spatial covariance at this band.
            std::fill(K.begin(), K.end(), std::complex<float>(0.0f, 0.0f));
            for (int t = 0; t < numFrames; ++t)
            {
                const auto& xt = snapshots[bi][(size_t) t];
                for (int r = 0; r < m_numChannels; ++r)
                {
                    const std::complex<float> xr = xt[(size_t) r];
                    for (int c = 0; c < m_numChannels; ++c)
                        K[(size_t) r * m_numChannels + c] += xr * std::conj(xt[(size_t) c]);
                }
            }
            const float invT = 1.0f / (float) numFrames;
            for (auto& v : K) v *= invT;

            computeSignalSubspace(K, maxSrc, signalSubspace);
            const int actualNumSrc = (int) signalSubspace.size();
            if (actualNumSrc == 0)
                continue;

            for (size_t gi = 0; gi < grid.size(); ++gi)
            {
                steeringVector(grid[gi], freqHz, a);

                float aNormSq = 0.0f;
                for (int m = 0; m < m_numChannels; ++m)
                    aNormSq += std::norm(a[(size_t) m]);

                // Noise-subspace projection via its orthogonal complement
                // (Parseval): ||E_n^H a||^2 = ||a||^2 - ||E_s^H a||^2 — only
                // the small signal subspace (size maxSrc) is needed, cheaper
                // and avoids picking specific representative vectors within
                // a degenerate noise-eigenvalue cluster.
                float signalProjSq = 0.0f;
                for (int s = 0; s < actualNumSrc; ++s)
                {
                    std::complex<float> dot(0.0f, 0.0f);
                    const auto& es = signalSubspace[(size_t) s];
                    for (int m = 0; m < m_numChannels; ++m)
                        dot += std::conj(es[(size_t) m]) * a[(size_t) m];
                    signalProjSq += std::norm(dot);
                }
                const float noiseProjSq = std::max(aNormSq - signalProjSq, 1e-12f);
                spectrum[gi] += 1.0 / (double) noiseProjSq;
            }
        }

        // ---- Peak selection: up to maxSrc positions, separated by a few
        //      grid steps so the same lobe isn't picked twice ----
        std::vector<size_t> order(grid.size());
        std::iota(order.begin(), order.end(), 0);
        std::sort(order.begin(), order.end(), [&](size_t x, size_t y) { return spectrum[x] > spectrum[y]; });

        const float minSep = gridRes * 4.0f;
        std::vector<GridPoint> selected;
        for (size_t idx : order)
        {
            const GridPoint& p = grid[idx];
            bool tooClose = false;
            for (const auto& s : selected)
            {
                const float dx = p.x - s.x, dy = p.y - s.y, dz = p.z - s.z;
                if (std::sqrt(dx * dx + dy * dy + dz * dz) < minSep) { tooClose = true; break; }
            }
            if (!tooClose)
            {
                selected.push_back(p);
                if ((int) selected.size() >= maxSrc)
                    break;
            }
        }

#if AT_SIXDOF_DEBUG_LOG
        std::cout << "[6DOF][localize] bins=" << binIndices.size() << "  frames=" << numFrames
                   << "  grid_points=" << grid.size() << "  peaks_selected=" << selected.size() << "\n";
        for (const auto& p : selected)
            std::cout << "[6DOF][localize]   peak @ (" << p.x << ", " << p.y << ", " << p.z << ")\n";
#endif

        // Plausibility filter (matches the previous implementation's
        // rationale — reject anything absurdly far from the array, e.g. a
        // numerical corner case rather than a real position).
        for (const auto& p : selected)
        {
            const float dx = p.x - m_arrayCentroid[0];
            const float dy = p.y - m_arrayCentroid[1];
            const float dz = p.z - m_arrayCentroid[2];
            if (std::sqrt(dx * dx + dy * dy + dz * dz) > m_maxPlausibleDist)
                continue;
            // Each peak feeds the SAME temporal history/hysteresis pipeline
            // as before, unchanged — a genuine source keeps landing near the
            // same grid point window after window; a pseudo-spectrum
            // artifact typically does not (see pushEstimateAndUpdateModes()).
            pushEstimateAndUpdateModes({ p.x, p.y, p.z });
        }
    }

    void SixDofMaskProcessor::pushEstimateAndUpdateModes(const SixDofSourcePosition& estimate)
    {
        m_estimateHistory[(size_t) m_historyWritePos] = estimate;
        m_historyWritePos = (m_historyWritePos + 1) % HISTORY_SIZE;
        m_historyCount = std::min(m_historyCount + 1, HISTORY_SIZE);

        // Mode search (histogram on a grid_res-rounded position) over the
        // rolling history — genuine dominant sources repeat almost exactly
        // from window to window, while spurious pseudo-spectrum peaks
        // (reverberation, finite-snapshot estimation noise) scatter and
        // rarely repeat. Counting occurrences therefore separates signal
        // from that specific kind of noise far more robustly than
        // proximity-based clustering (k-means).
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
