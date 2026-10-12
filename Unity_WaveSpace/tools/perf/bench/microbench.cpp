// at_microbench — plafond théorique du calcul WFS sur CETTE machine, sans le moteur.
//
// Mesure, pour N sources × C canaux, le coût de noyaux représentatifs :
//   A. lignes à retard fractionnaires (interp. linéaire) + gain lissé par échantillon, sommées sur C sorties
//   B. idem + 1 biquad par paire (source, HP)  — pire cas NFC « par paire »
//   C. idem B avec une entrée qui décroît vers le silence → dénormaux, FTZ/DAZ OFF puis ON
//
// Résultat : % du budget temps réel d'un bloc. Si A/B sont à quelques % pour N=16 alors que
// le moteur décroche à N=8–10, le problème n'est pas le volume de calcul (cf. CLAUDE.md).
//
// Sur CPU hybride (Windows), chaque série est jouée sur un P-core puis sur un E-core.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#if defined(_WIN32)
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
#endif
#if defined(_M_X64) || defined(__x86_64__)
  #include <xmmintrin.h>
  #define MB_X86 1
#else
  #define MB_X86 0
#endif

using clk = std::chrono::steady_clock;

static void setFtzDaz (bool on)
{
#if MB_X86
    unsigned int csr = _mm_getcsr();
    _mm_setcsr (on ? (csr | 0x8040u) : (csr & ~0x8040u));
#else
    (void) on;
#endif
}

struct Biquad
{
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    void setShelf()   // passe-haut doux, pôles proches du cercle → longues queues (pire cas dénormaux)
    {
        b0 = 0.9995f; b1 = -1.999f; b2 = 0.9995f; a1 = -1.9989f; a2 = 0.99895f;
    }
    inline float process (float x)
    {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

struct Engine
{
    int N, C, B, maxDelay;
    std::vector<std::vector<float>> delayLine;  // une ligne par source
    std::vector<int> writePos;
    std::vector<float> delayCur, delayTgt, gainCur, gainTgt; // [N*C]
    std::vector<Biquad> bq;                                  // [N*C]
    std::vector<std::vector<float>> in, out;

    Engine (int n, int c, int b) : N (n), C (c), B (b), maxDelay (8192)
    {
        delayLine.assign (N, std::vector<float> (maxDelay, 0.f));
        writePos.assign (N, 0);
        std::mt19937 rng (42);
        std::uniform_real_distribution<float> d (10.f, 900.f), g (0.05f, 1.f);
        delayCur.resize (N * C); delayTgt.resize (N * C); gainCur.resize (N * C); gainTgt.resize (N * C);
        for (int i = 0; i < N * C; ++i) { delayCur[i] = d (rng); delayTgt[i] = delayCur[i] + 0.5f; gainCur[i] = g (rng); gainTgt[i] = gainCur[i] * 0.99f; }
        bq.resize (N * C); for (auto& q : bq) q.setShelf();
        in.assign (N, std::vector<float> (B, 0.f));
        out.assign (C, std::vector<float> (B, 0.f));
    }

    void fillNoise (std::mt19937& rng)
    {
        std::uniform_real_distribution<float> u (-0.5f, 0.5f);
        for (auto& ch : in) for (auto& x : ch) x = u (rng);
    }

    void fillDecay (int blockIndex)
    {
        // décroissance exponentielle jusqu'à un plancher dénormal : modélise un gain lissé qui tend
        // vers 0 (fade-out, source masquée, atténuation distance) multiplié par le signal,
        // ou une queue de filtre IIR qui ne retombe jamais exactement à zéro.
        for (int s = 0; s < N; ++s)
            for (int i = 0; i < B; ++i)
            {
                const double t = (blockIndex * B + i) / 48000.0;
                const double amp = std::max (0.5 * std::exp (-t * 800.0), 1e-41);
                in[s][i] = (float) (amp * (((i * 2654435761u + s) >> 7) & 1 ? 1.0 : -1.0));
            }
    }

    void process (bool withBiquad)
    {
        for (auto& o : out) std::fill (o.begin(), o.end(), 0.f);
        const float inv = 1.f / (float) B;
        for (int s = 0; s < N; ++s)
        {
            auto& dl = delayLine[s];
            int wp = writePos[s];
            for (int i = 0; i < B; ++i) { dl[wp] = in[s][i]; wp = (wp + 1) % maxDelay; }

            for (int c = 0; c < C; ++c)
            {
                const int k = s * C + c;
                float d = delayCur[k], g = gainCur[k];
                const float d0 = d, g0 = g;
                const float dInc = (delayTgt[k] - d) * inv, gInc = (gainTgt[k] - g) * inv;
                float* o = out[c].data();
                Biquad& q = bq[k];
                int rp0 = writePos[s];
                for (int i = 0; i < B; ++i)
                {
                    float pos = (float) (rp0 + i) - d;
                    while (pos < 0) pos += (float) maxDelay;
                    const int i0 = (int) pos; const float fr = pos - (float) i0;
                    const float x0 = dl[i0 % maxDelay], x1 = dl[(i0 + 1) % maxDelay];
                    float y = (x0 + fr * (x1 - x0)) * g;
                    if (withBiquad) y = q.process (y);
                    o[i] += y;
                    d += dInc; g += gInc;
                }
                delayCur[k] = d; gainCur[k] = g;
                delayTgt[k] = d0; gainTgt[k] = g0;   // aller-retour : lissage permanent (cas source en mouvement)
            }
            writePos[s] = wp;
        }
    }
};

struct Result { double meanUs, p99Us, maxUs; };

static Result run (int N, int C, int B, int mode, bool ftz, int blocks)
{
    setFtzDaz (ftz);
    Engine e (N, C, B);
    std::mt19937 rng (1);
    std::vector<double> t; t.reserve (blocks);
    for (int b = 0; b < blocks + 50; ++b)
    {
        if (mode == 2) e.fillDecay (b); else e.fillNoise (rng);
        const auto t0 = clk::now();
        e.process (mode >= 1);
        const double us = std::chrono::duration<double, std::micro> (clk::now() - t0).count();
        if (b >= 50) t.push_back (us);   // ignore la chauffe
    }
    setFtzDaz (false);
    double sum = 0; for (double x : t) sum += x;
    std::sort (t.begin(), t.end());
    return { sum / t.size(), t[(size_t) (0.99 * (t.size() - 1))], t.back() };
}

// ---------------------------------------------------------------------------------
struct CoreSel { std::string name; uint64_t mask; };

static std::vector<CoreSel> coreSelections()
{
    std::vector<CoreSel> sel;
#if defined(_WIN32)
    ULONG len = 0;
    GetSystemCpuSetInformation (nullptr, 0, &len, GetCurrentProcess(), 0);
    std::vector<uint8_t> buf (len);
    int maxEc = 0; int pIdx = -1, eIdx = -1;
    if (GetSystemCpuSetInformation (reinterpret_cast<SYSTEM_CPU_SET_INFORMATION*> (buf.data()), len, &len, GetCurrentProcess(), 0))
    {
        for (uint8_t* p = buf.data(); p < buf.data() + len; p += reinterpret_cast<SYSTEM_CPU_SET_INFORMATION*> (p)->Size)
        {
            auto* e = reinterpret_cast<SYSTEM_CPU_SET_INFORMATION*> (p);
            if (e->Type == CpuSetInformation && e->CpuSet.Group == 0) maxEc = std::max (maxEc, (int) e->CpuSet.EfficiencyClass);
        }
        for (uint8_t* p = buf.data(); p < buf.data() + len; p += reinterpret_cast<SYSTEM_CPU_SET_INFORMATION*> (p)->Size)
        {
            auto* e = reinterpret_cast<SYSTEM_CPU_SET_INFORMATION*> (p);
            if (e->Type != CpuSetInformation || e->CpuSet.Group != 0) continue;
            const int li = e->CpuSet.LogicalProcessorIndex;
            if (e->CpuSet.EfficiencyClass == maxEc && pIdx < 0 && li > 0) pIdx = li; // évite le cœur 0 (interruptions)
            if (e->CpuSet.EfficiencyClass < maxEc && eIdx < 0) eIdx = li;
        }
    }
    if (maxEc > 0 && pIdx >= 0 && eIdx >= 0)
    {
        sel.push_back ({ "P-core #" + std::to_string (pIdx), 1ull << pIdx });
        sel.push_back ({ "E-core #" + std::to_string (eIdx), 1ull << eIdx });
        return sel;
    }
#endif
    sel.push_back ({ "cœur courant", 0 });
    return sel;
}

static void pin (uint64_t mask)
{
#if defined(_WIN32)
    if (mask) SetThreadAffinityMask (GetCurrentThread(), (DWORD_PTR) mask);
    SetThreadPriority (GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
#else
    (void) mask;
#endif
}

int main (int argc, char** argv)
{
    int C = 32, B = 512, blocks = 400; double fs = 48000;
    std::vector<int> Ns { 1, 4, 8, 10, 16, 32, 64 };
    for (int i = 1; i + 1 < argc; i += 2)
    {
        std::string k = argv[i];
        if (k == "--ch") C = std::atoi (argv[i + 1]);
        else if (k == "--buffer") B = std::atoi (argv[i + 1]);
        else if (k == "--blocks") blocks = std::atoi (argv[i + 1]);
        else if (k == "--fs") fs = std::atof (argv[i + 1]);
    }
    const double budgetUs = 1e6 * B / fs;
    std::printf ("Micro-bench WFS : %d canaux, bloc %d @ %.0f Hz → budget %.0f µs\n", C, B, fs, budgetUs);
#if defined(_DEBUG) || ! defined(NDEBUG)
    std::printf ("!!! ATTENTION : binaire non Release — chiffres non représentatifs !!!\n");
#endif

    struct Mode { int id; bool ftz; const char* name; };
    const Mode modes[] = {
        { 0, false, "A retard+gain" },
        { 1, false, "B +biquad/paire" },
        { 2, false, "C dénormaux FTZ off" },
        { 2, true,  "C dénormaux FTZ on" },
    };

    for (auto& cs : coreSelections())
    {
        pin (cs.mask);
        std::printf ("\n=== %s ===\n%-22s", cs.name.c_str(), "noyau \\ N sources");
        for (int n : Ns) std::printf ("%10d", n);
        std::printf ("\n");
        for (auto& m : modes)
        {
            std::printf ("%-22s", m.name);
            for (int n : Ns)
            {
                const auto r = run (n, C, B, m.id, m.ftz, blocks);
                std::printf ("%9.1f%%", 100.0 * r.meanUs / budgetUs);
            }
            std::printf ("\n%-22s", "   pic (max)");
            for (int n : Ns)
            {
                const auto r = run (n, C, B, m.id, m.ftz, blocks / 2);
                std::printf ("%9.1f%%", 100.0 * r.maxUs / budgetUs);
            }
            std::printf ("\n");
        }
    }
    std::printf ("\nLecture : charge moyenne en %% du budget d'un bloc sur UN cœur.\n"
                 "Écart « FTZ off » ≫ « FTZ on » ⇒ les dénormaux coûtent cher sur ce CPU (H2).\n"
                 "Écart P-core / E-core important ⇒ l'ordonnancement du thread audio compte (H6).\n");
    return 0;
}
