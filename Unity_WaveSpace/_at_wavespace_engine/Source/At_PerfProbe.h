// At_PerfProbe.h — instrumentation temps réel pour AT WaveSpace
// -----------------------------------------------------------------------------
// Tout est actif uniquement si AT_SPAT_PERF_PROBE est défini. Sinon, ce fichier
// ne déclare rien d'autre que des macros vides et le moteur est strictement inchangé.
//
// Côté thread audio : uniquement des atomiques relaxées, pas d'allocation,
// pas de lock, pas d'appel système bloquant.
//
// Deux copies de ce fichier doivent rester IDENTIQUES (StatsC est partagée par ABI) :
//   tools/perf/engine/At_PerfProbe.h           (référence, incluse par le bench)
//   _at_wavespace_engine/Source/At_PerfProbe.h (incluse par le moteur)
//
// Usage dans le callback (voir PATCH.md) :
//
//   void SpatializationEngine::getNextAudioBlock (const juce::AudioSourceChannelInfo& info)
//   {
//       AT_PERF_CALLBACK_SCOPE (info.numSamples, m_sampleRate);
//       AT_PERF_FTZ_IF_REQUESTED();
//       ...
//       { AT_PERF_SECTION (at_perf::Section::Players); /* lecture fichiers */ }
//       { AT_PERF_SECTION (at_perf::Section::WFS);     /* délais + gains   */ }
//       ...
//   }
// -----------------------------------------------------------------------------
#pragma once

#if defined(AT_SPAT_PERF_PROBE)

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>

// Pas de <windows.h> ici : ses macros (near, far, small, min/max…) n'ont rien à faire dans
// les unités de compilation du moteur. On déclare seulement les trois fonctions utilisées.
#if defined(_WIN32) && !defined(_WINDOWS_)
extern "C"
{
    __declspec(dllimport) unsigned long __stdcall GetCurrentProcessorNumber (void);
    __declspec(dllimport) void*         __stdcall GetCurrentThread (void);
    __declspec(dllimport) int           __stdcall GetThreadPriority (void*);
}
#endif

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
  #include <xmmintrin.h>
  #define AT_PERF_X86 1
#else
  #define AT_PERF_X86 0
#endif

#if !defined(_WIN32) && defined(__linux__)
  #include <sched.h>
#endif

namespace at_perf
{
    // Sections chronométrées à l'intérieur du callback.
    // sectionMeanUs = temps cumulé de la section PAR CALLBACK (tous threads confondus : en mode
    // multithread c'est donc du temps CPU, qui peut dépasser la durée du callback).
    // sectionMaxUs  = plus longue occurrence isolée.
    enum class Section : int
    {
        Players = 0,   // lecture fichiers / décodage / resampling / préfiltre (SpatPlayer::updateForNextBlock)
        WFS,           // boucle WFS par joueur : masque + délais/gains + ligne à retard (SpatPlayer::processAndAdd)
        NFC,           // filtres near-field
        Mask,          // réservé (le masque est calculé par échantillon dans la boucle WFS : non chronométrable à part)
        HRTF,          // binaural (devrait être ~0 en WFS pur)
        Output,        // fade de transition, master, vumètres
        Locks,         // acquisition des SpinLock du callback + la copie protégée (quelques Ko)
        Other,         // destruction différée des joueurs retirés
        MtDispatch,    // ThreadPool::addJob pour chaque joueur (allocation + lock du pool)
        MtWait,        // attente active du thread audio sur les workers
        Count
    };

    inline const char* sectionName (int i)
    {
        static const char* names[] = { "players", "wfs", "nfc", "mask", "hrtf", "output", "locks", "other",
                                       "mt_dispatch", "mt_wait" };
        return (i >= 0 && i < (int) Section::Count) ? names[i] : "?";
    }

    // Kill switches pour bissecter. Le moteur les lit (relaxed) aux endroits indiqués
    // dans PATCH.md. Valeurs : bitmask.
    enum Option : uint32_t
    {
        OPT_FORCE_FTZ_DAZ        = 1u << 0,  // forcer FTZ+DAZ sur le thread audio et les workers
        OPT_BYPASS_NFC           = 1u << 1,
        OPT_BYPASS_MASK_NORM     = 1u << 2,  // masque HP forcé à « tous actifs » (pas de classification)
        OPT_FREEZE_WFS_PARAMS    = 1u << 3,  // ne plus recalculer délais/gains (garder les derniers)
        OPT_MUTE_LOG_AUDIO       = 1u << 4,  // supprimer tout log émis depuis le thread audio
        OPT_SKIP_FILE_READ       = 1u << 5,  // remplacer la lecture fichier par un bruit interne
        OPT_NO_POSITION_LOCK     = 1u << 6,  // lire les positions sans lock (test seulement, peut glitcher)
        OPT_NO_MULTITHREADING    = 1u << 7,  // traiter tous les joueurs sur le thread audio
        OPT_WFS_PARAMS_PER_BLOCK = 1u << 8,  // masque + délais/gains une fois par bloc au lieu de par échantillon
        OPT_GLOBAL_SMOOTHERS_PER_BLOCK = 1u << 9,  // les joueurs n'avancent plus les lisseurs globaux du moteur (état partagé)
        OPT_FORCE_PLAYER_MT      = 1u << 10, // multithreading des joueurs forcé (désactivé par défaut depuis oct. 2026)
    };

    static constexpr int kHistBins   = 16;  // tranches de 10 % de charge, dernière = >150 %
    static constexpr int kMaxCpuBins = 64;

    // Structure POD exportée via l'API C. Garder l'ordre identique côté bench.
    struct StatsC
    {
        uint32_t version;              // = 2
        uint32_t blockSize;            // numSamples du dernier callback (taille RÉELLE)
        double   sampleRate;
        uint64_t callbacks;
        uint64_t overruns;             // callback > 100 % du budget
        uint64_t nearOverruns;         // callback > 70 % du budget
        double   budgetUs;             // blockSize / fs
        double   lastUs, meanUs, maxUs;
        double   maxIntervalUs;        // max écart entre deux débuts de callback
        double   maxIntervalJitterUs;  // max |intervalle - budget|
        uint64_t hist[kHistBins];      // distribution de charge
        double   sectionMeanUs[(int) Section::Count];
        double   sectionMaxUs [(int) Section::Count];
        uint32_t lastCpu;              // cœur logique du dernier callback
        uint32_t cpuMigrations;        // nb de changements de cœur
        uint64_t cpuHist[kMaxCpuBins]; // callbacks par cœur logique
        uint64_t denormalHits;         // callbacks où le flag DE (MXCSR) a été levé (thread audio seulement)
        uint64_t ftzDazOffCallbacks;   // callbacks exécutés sans FTZ+DAZ
        int32_t  threadPriority;       // GetThreadPriority (Windows)
        uint32_t options;              // options actives
        uint64_t logCallsFromAudio;    // log émis depuis le thread audio
    };

    class Probe
    {
    public:
        static Probe& get() { static Probe p; return p; }

        void reset()
        {
            m_resetRequested.store (true, std::memory_order_relaxed);
        }

        void setOptions (uint32_t o) { m_options.store (o, std::memory_order_relaxed); }
        uint32_t options() const     { return m_options.load (std::memory_order_relaxed); }
        bool has (Option o) const    { return (options() & o) != 0; }

        void noteLogFromAudioThread() { m_logCalls.fetch_add (1, std::memory_order_relaxed); }
        void noteDenormalHit()        { m_denormalHits.fetch_add (1, std::memory_order_relaxed); }

        bool isAudioThread() const
        {
            return m_audioThreadId.load (std::memory_order_relaxed) == std::this_thread::get_id();
        }

        // ---- appelé par le thread audio ------------------------------------------------
        void beginCallback (int numSamples, double sampleRate)
        {
            if (m_resetRequested.exchange (false, std::memory_order_relaxed))
                clearAudioSide();

            m_audioThreadId.store (std::this_thread::get_id(), std::memory_order_relaxed);

            const auto now = clock::now();
            if (m_hasLastStart)
            {
                const double intervalUs = toUs (now - m_lastStart);
                const double budgetUs   = sampleRate > 0 ? 1e6 * numSamples / sampleRate : 0.0;
                storeMax (m_maxIntervalUs, intervalUs);
                const double jitter = intervalUs > budgetUs ? intervalUs - budgetUs : budgetUs - intervalUs;
                storeMax (m_maxJitterUs, jitter);
            }
            m_lastStart = now;
            m_hasLastStart = true;
            m_cbStart = now;
            m_blockSize.store ((uint32_t) numSamples, std::memory_order_relaxed);
            m_sampleRate.store (sampleRate, std::memory_order_relaxed);

            // cœur courant
            const uint32_t cpu = currentCpu();
            const uint32_t prev = m_lastCpu.exchange (cpu, std::memory_order_relaxed);
            if (prev != cpu && prev != 0xFFFFFFFFu)
                m_cpuMigrations.fetch_add (1, std::memory_order_relaxed);
            m_cpuHist[cpu % kMaxCpuBins].fetch_add (1, std::memory_order_relaxed);

          #if AT_PERF_X86
            unsigned int csr = _mm_getcsr();
            if ((csr & 0x8040u) != 0x8040u) // FTZ (bit15) + DAZ (bit6)
                m_ftzDazOff.fetch_add (1, std::memory_order_relaxed);
            _mm_setcsr (csr & ~0x3Fu); // efface les flags d'exception collants
          #endif

          #if defined(_WIN32)
            m_threadPriority.store (GetThreadPriority (GetCurrentThread()), std::memory_order_relaxed);
          #endif
        }

        void endCallback()
        {
          #if AT_PERF_X86
            if (_mm_getcsr() & 0x2u) // DE : opérande dénormale rencontrée
                m_denormalHits.fetch_add (1, std::memory_order_relaxed);
          #endif
            const double us = toUs (clock::now() - m_cbStart);
            const double fs = m_sampleRate.load (std::memory_order_relaxed);
            const uint32_t n = m_blockSize.load (std::memory_order_relaxed);
            const double budgetUs = fs > 0 ? 1e6 * n / fs : 1.0;

            const uint64_t c = m_callbacks.fetch_add (1, std::memory_order_relaxed) + 1;
            m_lastUs.store (us, std::memory_order_relaxed);
            storeMax (m_maxUs, us);
            // moyenne incrémentale (un seul écrivain)
            const double mean = m_meanUs.load (std::memory_order_relaxed);
            m_meanUs.store (mean + (us - mean) / (double) c, std::memory_order_relaxed);

            const double load = us / budgetUs;
            if (load > 1.0) m_overruns.fetch_add (1, std::memory_order_relaxed);
            if (load > 0.7) m_nearOverruns.fetch_add (1, std::memory_order_relaxed);
            int bin = (int) (load * 10.0);
            if (bin >= kHistBins) bin = kHistBins - 1;
            m_hist[bin].fetch_add (1, std::memory_order_relaxed);
        }

        // ---- appelé par le thread audio ET par les workers (plusieurs écrivains) --------
        void addSection (Section s, double us)
        {
            const int i = (int) s;
            m_secSumNs[i].fetch_add ((uint64_t) (us * 1000.0), std::memory_order_relaxed);
            storeMax (m_secMaxUs[i], us);
        }

        // ---- appelé par n'importe quel thread (lecture approximative, suffisante) ---------
        void snapshot (StatsC& s) const
        {
            std::memset (&s, 0, sizeof (s));
            s.version     = 2;
            s.blockSize   = m_blockSize.load (std::memory_order_relaxed);
            s.sampleRate  = m_sampleRate.load (std::memory_order_relaxed);
            s.callbacks   = m_callbacks.load (std::memory_order_relaxed);
            s.overruns    = m_overruns.load (std::memory_order_relaxed);
            s.nearOverruns= m_nearOverruns.load (std::memory_order_relaxed);
            s.budgetUs    = s.sampleRate > 0 ? 1e6 * s.blockSize / s.sampleRate : 0.0;
            s.lastUs      = m_lastUs.load (std::memory_order_relaxed);
            s.meanUs      = m_meanUs.load (std::memory_order_relaxed);
            s.maxUs       = m_maxUs.load (std::memory_order_relaxed);
            s.maxIntervalUs       = m_maxIntervalUs.load (std::memory_order_relaxed);
            s.maxIntervalJitterUs = m_maxJitterUs.load (std::memory_order_relaxed);
            for (int i = 0; i < kHistBins; ++i) s.hist[i] = m_hist[i].load (std::memory_order_relaxed);
            for (int i = 0; i < (int) Section::Count; ++i)
            {
                s.sectionMeanUs[i] = s.callbacks > 0
                                   ? (double) m_secSumNs[i].load (std::memory_order_relaxed) / 1000.0 / (double) s.callbacks
                                   : 0.0;
                s.sectionMaxUs[i]  = m_secMaxUs[i].load (std::memory_order_relaxed);
            }
            s.lastCpu       = m_lastCpu.load (std::memory_order_relaxed);
            s.cpuMigrations = m_cpuMigrations.load (std::memory_order_relaxed);
            for (int i = 0; i < kMaxCpuBins; ++i) s.cpuHist[i] = m_cpuHist[i].load (std::memory_order_relaxed);
            s.denormalHits       = m_denormalHits.load (std::memory_order_relaxed);
            s.ftzDazOffCallbacks = m_ftzDazOff.load (std::memory_order_relaxed);
            s.threadPriority     = m_threadPriority.load (std::memory_order_relaxed);
            s.options            = options();
            s.logCallsFromAudio  = m_logCalls.load (std::memory_order_relaxed);
        }

    private:
        using clock = std::chrono::steady_clock; // QueryPerformanceCounter sous MSVC

        static double toUs (clock::duration d)
        {
            return std::chrono::duration<double, std::micro> (d).count();
        }

        static void storeMax (std::atomic<double>& a, double v)
        {
            double cur = a.load (std::memory_order_relaxed);
            while (v > cur && ! a.compare_exchange_weak (cur, v, std::memory_order_relaxed)) {}
        }

        static uint32_t currentCpu()
        {
          #if defined(_WIN32)
            return (uint32_t) GetCurrentProcessorNumber();
          #elif defined(__linux__)
            const int c = sched_getcpu();
            return c < 0 ? 0u : (uint32_t) c;
          #else
            return 0u;
          #endif
        }

        void clearAudioSide()
        {
            m_callbacks = 0; m_overruns = 0; m_nearOverruns = 0;
            m_lastUs = 0; m_meanUs = 0; m_maxUs = 0;
            m_maxIntervalUs = 0; m_maxJitterUs = 0; m_hasLastStart = false;
            for (auto& h : m_hist) h = 0;
            for (int i = 0; i < (int) Section::Count; ++i) { m_secSumNs[i] = 0; m_secMaxUs[i] = 0; }
            m_cpuMigrations = 0; m_lastCpu = 0xFFFFFFFFu;
            for (auto& h : m_cpuHist) h = 0;
            m_denormalHits = 0; m_ftzDazOff = 0; m_logCalls = 0;
        }

        std::atomic<bool>     m_resetRequested { false };
        std::atomic<uint32_t> m_options { 0 };
        std::atomic<std::thread::id> m_audioThreadId {};

        clock::time_point m_lastStart {}, m_cbStart {};
        bool m_hasLastStart = false;

        std::atomic<uint32_t> m_blockSize { 0 };
        std::atomic<double>   m_sampleRate { 0 };
        std::atomic<uint64_t> m_callbacks { 0 }, m_overruns { 0 }, m_nearOverruns { 0 };
        std::atomic<double>   m_lastUs { 0 }, m_meanUs { 0 }, m_maxUs { 0 };
        std::atomic<double>   m_maxIntervalUs { 0 }, m_maxJitterUs { 0 };
        std::atomic<uint64_t> m_hist[kHistBins] {};
        std::atomic<uint64_t> m_secSumNs[(int) Section::Count] {};
        std::atomic<double>   m_secMaxUs [(int) Section::Count] {};
        std::atomic<uint32_t> m_lastCpu { 0xFFFFFFFFu }, m_cpuMigrations { 0 };
        std::atomic<uint64_t> m_cpuHist[kMaxCpuBins] {};
        std::atomic<uint64_t> m_denormalHits { 0 }, m_ftzDazOff { 0 }, m_logCalls { 0 };
        std::atomic<int32_t>  m_threadPriority { 0 };
    };

    // RAII
    struct CallbackScope
    {
        CallbackScope (int n, double fs) { Probe::get().beginCallback (n, fs); }
        ~CallbackScope()                 { Probe::get().endCallback(); }
    };

    struct SectionScope
    {
        explicit SectionScope (Section s) : sec (s), t0 (std::chrono::steady_clock::now()) {}
        ~SectionScope()
        {
            Probe::get().addSection (sec, std::chrono::duration<double, std::micro> (
                                              std::chrono::steady_clock::now() - t0).count());
        }
        Section sec;
        std::chrono::steady_clock::time_point t0;
    };

    struct FtzDazScope
    {
        FtzDazScope()
        {
          #if AT_PERF_X86
            active = Probe::get().has (OPT_FORCE_FTZ_DAZ);
            if (active) { saved = _mm_getcsr(); _mm_setcsr (saved | 0x8040u); }
          #endif
        }
        ~FtzDazScope()
        {
          #if AT_PERF_X86
            // restaure FTZ/DAZ d'origine mais conserve les flags levés pendant le traitement
            if (active) _mm_setcsr ((saved & ~0x3Fu) | (_mm_getcsr() & 0x3Fu));
          #endif
        }
        bool active = false;
        unsigned int saved = 0;
    };

    // À placer en tête de chaque job exécuté par un thread worker : l'état FTZ/DAZ et le flag DE
    // sont PAR THREAD, donc invisibles depuis le thread audio. Applique FTZ/DAZ sur demande et
    // compte un denormalHit par job ayant rencontré une opérande dénormale.
    struct WorkerScope
    {
        WorkerScope()
        {
          #if AT_PERF_X86
            active = ! Probe::get().isAudioThread();
            if (! active) return;
            saved = _mm_getcsr();
            unsigned int csr = saved & ~0x3Fu;
            if (Probe::get().has (OPT_FORCE_FTZ_DAZ)) csr |= 0x8040u;
            _mm_setcsr (csr);
          #endif
        }
        ~WorkerScope()
        {
          #if AT_PERF_X86
            if (! active) return;
            if (_mm_getcsr() & 0x2u) Probe::get().noteDenormalHit();
            _mm_setcsr (saved);
          #endif
        }
        bool active = false;
        unsigned int saved = 0;
    };

    // Verrou RAII pris seulement si `take` est vrai (kill switch OPT_NO_POSITION_LOCK).
    template <typename LockType>
    struct OptionalScopedLock
    {
        OptionalScopedLock (LockType& l, bool take) : lock (l), taken (take) { if (taken) lock.enter(); }
        ~OptionalScopedLock()                                                { if (taken) lock.exit(); }
        LockType& lock;
        const bool taken;
    };
} // namespace at_perf

  #define AT_PERF_CONCAT2(a, b) a##b
  #define AT_PERF_CONCAT(a, b)  AT_PERF_CONCAT2(a, b)

  #define AT_PERF_CALLBACK_SCOPE(n, fs) at_perf::CallbackScope AT_PERF_CONCAT(_atPerfCb_, __LINE__) ((int) (n), (double) (fs))
  #define AT_PERF_SECTION(sec)          at_perf::SectionScope  AT_PERF_CONCAT(_atPerfSec_, __LINE__) (sec)
  #define AT_PERF_FTZ_IF_REQUESTED()    at_perf::FtzDazScope   AT_PERF_CONCAT(_atPerfFtz_, __LINE__)
  #define AT_PERF_WORKER_SCOPE()        at_perf::WorkerScope   AT_PERF_CONCAT(_atPerfWrk_, __LINE__)
  #define AT_PERF_OPTION(opt)           (at_perf::Probe::get().has (at_perf::opt))
  #define AT_PERF_NOTE_AUDIO_LOG()      at_perf::Probe::get().noteLogFromAudioThread()
  // Remplace « const juce::SpinLock::ScopedLockType lock (m_positionLock); » dans le callback.
  #define AT_PERF_POSITION_LOCK(lockObj) \
      at_perf::OptionalScopedLock<juce::SpinLock> AT_PERF_CONCAT(_atPerfLock_, __LINE__) ((lockObj), ! AT_PERF_OPTION (OPT_NO_POSITION_LOCK))
  // En tête des fonctions de log : compte les logs émis par le thread audio, et les supprime sur demande.
  #define AT_PERF_AUDIO_LOG_GUARD() \
      do { if (at_perf::Probe::get().isAudioThread()) { AT_PERF_NOTE_AUDIO_LOG(); if (AT_PERF_OPTION (OPT_MUTE_LOG_AUDIO)) return; } } while (0)

#else // AT_SPAT_PERF_PROBE absent : rien n'est compilé

  #define AT_PERF_CALLBACK_SCOPE(n, fs)  ((void) 0)
  #define AT_PERF_SECTION(sec)           ((void) 0)
  #define AT_PERF_FTZ_IF_REQUESTED()     ((void) 0)
  #define AT_PERF_WORKER_SCOPE()         ((void) 0)
  #define AT_PERF_OPTION(opt)            (false)
  #define AT_PERF_NOTE_AUDIO_LOG()       ((void) 0)
  #define AT_PERF_POSITION_LOCK(lockObj) const juce::SpinLock::ScopedLockType lock (lockObj)
  #define AT_PERF_AUDIO_LOG_GUARD()      ((void) 0)

#endif
