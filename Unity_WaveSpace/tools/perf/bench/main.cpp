// at_spat_bench — banc de charge console pour AT WaveSpace (WFS)
//
// Exemple :
//   at_spat_bench --dll at_wavespace_engine.dll --device "MADIface USB" --out 32 --buffer 512 ^
//                 --sources 1,2,4,6,8,10,12,16 --duration 20 --file signals\pink_10s.wav ^
//                 --motion orbit --update-hz 60 --csv results\run.csv --label asio512_orbit
//
//   at_spat_bench --sysinfo        (cœurs P/E, EcoQoS, priorité — sans charger la DLL)
//
// Voir tools/perf/PERF.md pour le protocole.

#include "at_spat_api.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
  #include <mmsystem.h>
  #pragma comment(lib, "winmm.lib")
#endif

using namespace std::chrono;

// ---------------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------------
struct Args
{
    std::string dll = "at_wavespace_engine.dll";
    std::string device;               // vide = périphérique par défaut du moteur
    int outCh = 32, inCh = 0, buffer = 512;
    std::vector<int> sources { 1, 2, 4, 6, 8, 10, 12, 16 };
    double duration = 20.0, warmup = 3.0;
    std::vector<std::string> files;
    std::string motion = "static";    // static | orbit | listener | jitter
    double updateHz = 60.0;
    double sourceZ = -1.0;            // < 0 : entre auditeur et réseau (géométrie d'évaluation)
    double listenerZ = -3.0;
    std::string csv, label = "run";
    std::string affinity = "all";     // all | pcore | ecore | <masque hex>
    std::string priority = "normal";  // normal | high | realtime
    bool noThrottle = false, timerRes = false, sysinfo = false, logVerbose = false;
    unsigned int perfOptions = 0;
    bool stopOnFirstGlitchStep = false;
    // Réglages moteur : défauts = scènes Unity habituelles (StreamingAssets/*_States.state)
    bool prefilter = true, wfsGain = true, minMax = true, mask = true;
    float sourceSize = 0.3f;
    float masterDb = -12.f;           // le moteur attend des dB (pow(10, g/20))
    // Sans interface multicanal : --out HP virtuels, rendus sur 2 sorties physiques (virtualisation binaurale)
    bool downmix = false;
    std::string render = "amp";       // amp (panoramique d'amplitude, quasi gratuit) | hrtf
    bool listDevices = false;
};

static std::vector<std::string> split (const std::string& s, char sep)
{
    std::vector<std::string> out; std::stringstream ss (s); std::string item;
    while (std::getline (ss, item, sep)) if (! item.empty()) out.push_back (item);
    return out;
}

static unsigned int parsePerfOptions (const std::string& s)
{
    unsigned int o = 0;
    for (auto& t : split (s, ','))
    {
        if      (t == "ftz")      o |= at_perf::OPT_FORCE_FTZ_DAZ;
        else if (t == "nonfc")    o |= at_perf::OPT_BYPASS_NFC;
        else if (t == "nomask")   o |= at_perf::OPT_BYPASS_MASK_NORM;
        else if (t == "freeze")   o |= at_perf::OPT_FREEZE_WFS_PARAMS;
        else if (t == "mutelog")  o |= at_perf::OPT_MUTE_LOG_AUDIO;
        else if (t == "noread")   o |= at_perf::OPT_SKIP_FILE_READ;
        else if (t == "nolock")   o |= at_perf::OPT_NO_POSITION_LOCK;
        else if (t == "nomt")     o |= at_perf::OPT_NO_MULTITHREADING;
        else if (t == "blockwfs") o |= at_perf::OPT_WFS_PARAMS_PER_BLOCK;
        else if (t == "gsblock")  o |= at_perf::OPT_GLOBAL_SMOOTHERS_PER_BLOCK;
        else if (t == "mt")       o |= at_perf::OPT_FORCE_PLAYER_MT;
        else std::fprintf (stderr, "Option perf inconnue : %s\n", t.c_str());
    }
    return o;
}

static void usage()
{
    std::puts (
        "at_spat_bench [options]\n"
        "  --dll PATH           DLL du moteur\n"
        "  --device NAME        nom du périphérique (tel que vu par le moteur)\n"
        "  --out N --in N       canaux (défaut 32 / 0)\n"
        "  --buffer N           taille demandée (vérifier blockSize réel dans la sortie)\n"
        "  --sources 1,2,4,...  paliers de nombre de sources\n"
        "  --duration S         durée de mesure par palier (défaut 20)\n"
        "  --warmup S           chauffe avant mesure (défaut 3)\n"
        "  --file a.wav,b.wav   fichiers (répartis en tourniquet sur les sources)\n"
        "  --motion M           static | orbit | listener | jitter\n"
        "  --update-hz F        fréquence d'envoi des transforms (0 = jamais après init)\n"
        "  --source-z Z         profondeur des sources (défaut -1, réseau en z=0)\n"
        "  --listener-z Z       défaut -3\n"
        "  --perf ftz,nonfc,nomask,freeze,mutelog,noread,nolock,nomt,blockwfs,gsblock,mt   kill switches (probe)\n"
        "  --prefilter 0|1      préfiltre WFS (défaut 1, comme les scènes Unity)\n"
        "  --wfs-gain 0|1       pondération d'amplitude WFS (défaut 1)\n"
        "  --minmax 0|1         min/max des délais sur HP actifs seulement (défaut 1)\n"
        "  --mask 0|1           masque d'activation des HP (défaut 1)\n"
        "  --source-size M      rayon des sources secondaires (défaut 0.3)\n"
        "  --master-db G        gain master en dB (défaut -12)\n"
        "  --downmix            --out HP virtuels rendus en stéréo (carte son 2 canaux)\n"
        "  --render amp|hrtf    rendu du downmix (défaut amp : ne mesure que la WFS)\n"
        "  --list-devices       liste les périphériques vus par le moteur puis quitte\n"
        "  --affinity all|pcore|ecore|0xMASK\n"
        "  --priority normal|high|realtime\n"
        "  --no-throttle        désactive EcoQoS / power throttling pour le process\n"
        "  --timer-res          timeBeginPeriod(1)\n"
        "  --csv PATH --label L sortie CSV (ajout)\n"
        "  --stop-on-glitch     arrête la montée après le premier palier avec overruns\n"
        "  --log-verbose        affiche les logs moteur (perturbe la mesure)\n"
        "  --sysinfo            infos système puis quitte\n");
}

static bool parseArgs (int argc, char** argv, Args& a)
{
    for (int i = 1; i < argc; ++i)
    {
        std::string k = argv[i];
        auto next = [&] () -> std::string { if (i + 1 >= argc) { std::fprintf (stderr, "Valeur manquante pour %s\n", k.c_str()); std::exit (2); } return argv[++i]; };
        if      (k == "--dll")          a.dll = next();
        else if (k == "--device")       a.device = next();
        else if (k == "--out")          a.outCh = std::atoi (next().c_str());
        else if (k == "--in")           a.inCh = std::atoi (next().c_str());
        else if (k == "--buffer")       a.buffer = std::atoi (next().c_str());
        else if (k == "--sources")      { a.sources.clear(); for (auto& s : split (next(), ',')) a.sources.push_back (std::atoi (s.c_str())); }
        else if (k == "--duration")     a.duration = std::atof (next().c_str());
        else if (k == "--warmup")       a.warmup = std::atof (next().c_str());
        else if (k == "--file")         a.files = split (next(), ',');
        else if (k == "--motion")       a.motion = next();
        else if (k == "--update-hz")    a.updateHz = std::atof (next().c_str());
        else if (k == "--source-z")     a.sourceZ = std::atof (next().c_str());
        else if (k == "--listener-z")   a.listenerZ = std::atof (next().c_str());
        else if (k == "--perf")         a.perfOptions = parsePerfOptions (next());
        else if (k == "--prefilter")    a.prefilter = std::atoi (next().c_str()) != 0;
        else if (k == "--wfs-gain")     a.wfsGain = std::atoi (next().c_str()) != 0;
        else if (k == "--minmax")       a.minMax = std::atoi (next().c_str()) != 0;
        else if (k == "--mask")         a.mask = std::atoi (next().c_str()) != 0;
        else if (k == "--source-size")  a.sourceSize = (float) std::atof (next().c_str());
        else if (k == "--master-db")    a.masterDb = (float) std::atof (next().c_str());
        else if (k == "--downmix")      a.downmix = true;
        else if (k == "--render")       a.render = next();
        else if (k == "--list-devices") a.listDevices = true;
        else if (k == "--affinity")     a.affinity = next();
        else if (k == "--priority")     a.priority = next();
        else if (k == "--no-throttle")  a.noThrottle = true;
        else if (k == "--timer-res")    a.timerRes = true;
        else if (k == "--csv")          a.csv = next();
        else if (k == "--label")        a.label = next();
        else if (k == "--stop-on-glitch") a.stopOnFirstGlitchStep = true;
        else if (k == "--log-verbose")  a.logVerbose = true;
        else if (k == "--sysinfo")      a.sysinfo = true;
        else if (k == "-h" || k == "--help") { usage(); return false; }
        else { std::fprintf (stderr, "Argument inconnu : %s\n", k.c_str()); usage(); return false; }
    }
    return true;
}

// ---------------------------------------------------------------------------------
// Système : cœurs P/E, affinité, EcoQoS, priorité
// ---------------------------------------------------------------------------------
struct CoreInfo { uint32_t logical; int efficiencyClass; int coreIndex; };

static std::vector<CoreInfo> queryCores()
{
    std::vector<CoreInfo> cores;
  #if defined(_WIN32)
    ULONG len = 0;
    GetSystemCpuSetInformation (nullptr, 0, &len, GetCurrentProcess(), 0);
    std::vector<uint8_t> buf (len);
    auto* info = reinterpret_cast<SYSTEM_CPU_SET_INFORMATION*> (buf.data());
    if (GetSystemCpuSetInformation (info, len, &len, GetCurrentProcess(), 0))
    {
        for (uint8_t* p = buf.data(); p < buf.data() + len; )
        {
            auto* e = reinterpret_cast<SYSTEM_CPU_SET_INFORMATION*> (p);
            if (e->Type == CpuSetInformation)
                cores.push_back ({ (uint32_t) e->CpuSet.LogicalProcessorIndex + 64u * e->CpuSet.Group,
                                   (int) e->CpuSet.EfficiencyClass, (int) e->CpuSet.CoreIndex });
            p += e->Size;
        }
    }
  #else
    const unsigned n = std::thread::hardware_concurrency();
    for (unsigned i = 0; i < n; ++i) cores.push_back ({ i, 0, (int) i });
  #endif
    return cores;
}

static int maxEfficiencyClass (const std::vector<CoreInfo>& c)
{
    int m = 0; for (auto& x : c) m = std::max (m, x.efficiencyClass); return m;
}

static void printSysInfo()
{
    auto cores = queryCores();
    const int maxEc = maxEfficiencyClass (cores);
    int nP = 0, nE = 0;
    for (auto& c : cores) (c.efficiencyClass == maxEc ? nP : nE)++;
    std::printf ("Cœurs logiques : %zu  |  hybride : %s  |  P (classe %d) : %d  |  E : %d\n",
                 cores.size(), (maxEc > 0 ? "OUI" : "non"), maxEc, nP, nE);
    std::printf ("  logique -> classe (0 = E-core sur CPU hybride ; plus grand = plus performant)\n  ");
    for (auto& c : cores) std::printf ("%u:%s ", c.logical, (maxEc > 0 ? (c.efficiencyClass == maxEc ? "P" : "E") : "-"));
    std::printf ("\n");
  #if defined(_WIN32)
    PROCESS_POWER_THROTTLING_STATE st {}; st.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    if (GetProcessInformation (GetCurrentProcess(), ProcessPowerThrottling, &st, sizeof (st)))
        std::printf ("Power throttling (EcoQoS) process : ControlMask=0x%lx StateMask=0x%lx\n", st.ControlMask, st.StateMask);
    std::printf ("Classe de priorité process : 0x%lx\n", GetPriorityClass (GetCurrentProcess()));
  #endif
}

static void applyProcessSettings (const Args& a)
{
  #if defined(_WIN32)
    if (a.noThrottle)
    {
        PROCESS_POWER_THROTTLING_STATE st {};
        st.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        st.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
        st.StateMask = 0; // 0 = jamais throttlé
        if (! SetProcessInformation (GetCurrentProcess(), ProcessPowerThrottling, &st, sizeof (st)))
            std::fprintf (stderr, "SetProcessInformation(PowerThrottling) a échoué (%lu)\n", GetLastError());
        else std::printf ("EcoQoS désactivé pour ce process\n");
    }
    if (a.timerRes) { timeBeginPeriod (1); std::printf ("timeBeginPeriod(1)\n"); }

    DWORD pc = NORMAL_PRIORITY_CLASS;
    if (a.priority == "high") pc = HIGH_PRIORITY_CLASS;
    else if (a.priority == "realtime") pc = REALTIME_PRIORITY_CLASS;
    if (pc != NORMAL_PRIORITY_CLASS && ! SetPriorityClass (GetCurrentProcess(), pc))
        std::fprintf (stderr, "SetPriorityClass a échoué (%lu)\n", GetLastError());

    if (a.affinity != "all")
    {
        DWORD_PTR mask = 0;
        if (a.affinity.rfind ("0x", 0) == 0) mask = (DWORD_PTR) std::strtoull (a.affinity.c_str(), nullptr, 16);
        else
        {
            auto cores = queryCores();
            const int maxEc = maxEfficiencyClass (cores);
            if (maxEc == 0) std::printf ("CPU non hybride : --affinity %s ignoré\n", a.affinity.c_str());
            for (auto& c : cores)
            {
                const bool isP = c.efficiencyClass == maxEc;
                if (c.logical < 64 && ((a.affinity == "pcore" && isP) || (a.affinity == "ecore" && ! isP)))
                    mask |= (DWORD_PTR) 1 << c.logical;
            }
        }
        if (mask && SetProcessAffinityMask (GetCurrentProcess(), mask))
            std::printf ("Affinité process = 0x%llx\n", (unsigned long long) mask);
        else std::fprintf (stderr, "Affinité non appliquée\n");
    }
  #else
    (void) a;
  #endif
}

static double processCpuSeconds()
{
  #if defined(_WIN32)
    FILETIME c, e, k, u;
    GetProcessTimes (GetCurrentProcess(), &c, &e, &k, &u);
    auto toS = [] (FILETIME f) { ULARGE_INTEGER x; x.LowPart = f.dwLowDateTime; x.HighPart = f.dwHighDateTime; return x.QuadPart * 1e-7; };
    return toS (k) + toS (u);
  #else
    return (double) std::clock() / CLOCKS_PER_SEC;
  #endif
}

// ---------------------------------------------------------------------------------
// Log moteur : comptage uniquement (afficher depuis le thread audio fausserait la mesure)
// ---------------------------------------------------------------------------------
static std::atomic<uint64_t> g_logCount { 0 };
static std::atomic<bool>     g_logVerbose { false };

static void onEngineLog (const char* msg)
{
    g_logCount.fetch_add (1, std::memory_order_relaxed);
    if (g_logVerbose.load (std::memory_order_relaxed) && msg) std::fprintf (stderr, "[moteur] %s\n", msg);
}

// ---------------------------------------------------------------------------------
// Géométrie (cf. géométrie d'évaluation : 32 HP × 18,5 cm, réseau en z = 0)
// ---------------------------------------------------------------------------------
struct Vec3 { float x, y, z; };

static void setSpeakers (AtSpatApi& api, int n)
{
    std::vector<float> pos (3 * n), rot (3 * n), fwd (3 * n);
    const float spacing = 0.185f, x0 = -0.5f * spacing * (n - 1);
    for (int i = 0; i < n; ++i)
    {
        pos[3*i] = x0 + spacing * i; pos[3*i+1] = 0.f; pos[3*i+2] = 0.f;
        rot[3*i] = 0.f; rot[3*i+1] = 180.f; rot[3*i+2] = 0.f;        // HP tournés vers l'auditeur (-z)
        fwd[3*i] = 0.f; fwd[3*i+1] = 0.f;  fwd[3*i+2] = -1.f;
    }
    if (api.setVirtualSpeakerTransform (pos.data(), rot.data(), fwd.data(), n) != 0)
        std::fprintf (stderr, "setVirtualSpeakerTransform a renvoyé une erreur\n");
}

static void setListener (AtSpatApi& api, Vec3 p)
{
    float pos[3] = { p.x, p.y, p.z }, rot[3] = { 0, 0, 0 }, fwd[3] = { 0, 0, 1 };
    api.setListenerTransform (pos, rot, fwd);
}

static Vec3 sourceBase (int i, int n, float z)
{
    const float span = 4.5f; // reste à l'intérieur du réseau (5,7 m)
    const float x = n == 1 ? 0.f : -0.5f * span + span * (float) i / (float) (n - 1);
    return { x, 0.f, z };
}

static void sendSource (AtSpatApi& api, int uid, Vec3 p)
{
    float pos[3] = { p.x, p.y, p.z }, rot[3] = { 0, 0, 0 }, fwd[3] = { 0, 0, 1 };
    api.setPlayerTransform (uid, pos, rot, fwd);
}

// ---------------------------------------------------------------------------------
// CSV
// ---------------------------------------------------------------------------------
static void writeCsvHeader (std::ofstream& f)
{
    f << "label,device,out_ch,buffer_req,motion,update_hz,perf_opts,affinity,priority,no_throttle,prefilter,wfs_gain,minmax,mask,downmix,"
         "n_sources,t_s,block_size,sample_rate,budget_us,callbacks,overruns,near_overruns,mean_us,max_us,"
         "max_interval_us,max_jitter_us,denormal_hits,ftz_daz_off,last_cpu,cpu_migrations,thread_prio,"
         "log_audio,log_total,proc_cpu_pct_of_one_core";
    for (int s = 0; s < (int) at_perf::Section::Count; ++s)
        f << ",sec_" << at_perf::sectionName (s) << "_mean_us,sec_" << at_perf::sectionName (s) << "_max_us";
    f << "\n";
}

// ---------------------------------------------------------------------------------
int main (int argc, char** argv)
{
    Args a;
    if (! parseArgs (argc, argv, a)) return 1;
    if (a.sysinfo) { printSysInfo(); return 0; }
    if (a.files.empty() && ! a.listDevices) { std::fprintf (stderr, "--file obligatoire (voir scripts/gen_test_signals.py)\n"); return 2; }

    applyProcessSettings (a);
    printSysInfo();
    g_logVerbose = a.logVerbose;

    AtSpatApi api;
    if (! api.load (a.dll)) return 3;
    std::printf ("Probe perf : %s\n", api.hasProbe() ? "présent" : "ABSENT (mode dégradé : CPU process uniquement)");

    if (api.setLogCallback) api.setLogCallback (&onEngineLog);
    if (api.initialize() != 0) { std::fprintf (stderr, "AT_WS_initialize a échoué\n"); return 4; }
    if (api.waitForDeviceScan) api.waitForDeviceScan (10000);

    if (a.listDevices)
    {
        // Rescan synchrone sur ce thread : le scan asynchrone du constructeur ne remonte pas les périphériques WASAPI ici.
        if (api.refreshDevices) api.refreshDevices (1, 0);
        const int n = api.getDeviceCount ? api.getDeviceCount() : 0;
        std::printf ("\n%d périphérique(s) vus par le moteur (-1 = nombre de canaux pas encore interrogé) :\n", n);
        for (int i = 0; i < n && api.getCachedDeviceInfo; ++i)
        {
            char name[256] = {}, type[64] = {}; int in = -1, out = -1;
            if (api.getCachedDeviceInfo (i, name, type, &in, &out) == 0)
                std::printf ("  [%2d] %-22s in=%-3d out=%-3d  \"%s\"\n", i, type, in, out, name);
        }
        api.shutdown(); return 0;
    }

    if (api.setup (a.device.empty() ? nullptr : a.device.c_str(), a.inCh, a.outCh, a.buffer, a.downmix) != 0)
    {
        std::fprintf (stderr, "AT_WS_setup a échoué (device='%s', out=%d, buffer=%d)\n", a.device.c_str(), a.outCh, a.buffer);
        api.shutdown(); return 5;
    }
    if (api.setIsSimpleBinauralSpat)     api.setIsSimpleBinauralSpat (false);
    if (a.downmix)
    {
        if (api.setBinauralRenderMode) api.setBinauralRenderMode (a.render == "hrtf" ? 1 : 0);
        std::printf ("Downmix stéréo : %d HP virtuels -> 2 sorties physiques, rendu %s\n", a.outCh, a.render.c_str());
    }
    if (api.setMasterGain)               api.setMasterGain (a.masterDb);
    // Mémorisés par le moteur et appliqués à chaque joueur ajouté ensuite.
    if (api.setIsPrefilterAllPlayers)    api.setIsPrefilterAllPlayers (a.prefilter);
    if (api.setIsWfsGain)                api.setIsWfsGain (a.wfsGain);
    if (api.setIsActiveSpeakersMinMax)   api.setIsActiveSpeakersMinMax (a.minMax);
    if (api.enableAllPlayersSpeakerMask) api.enableAllPlayersSpeakerMask (a.mask);
    if (api.setSecondarySourceSize)      api.setSecondarySourceSize (a.sourceSize);
    std::printf ("Moteur : prefilter=%d wfsGain=%d minMax=%d mask=%d sourceSize=%.2f master=%.1f dB\n",
                 a.prefilter, a.wfsGain, a.minMax, a.mask, a.sourceSize, a.masterDb);

    setSpeakers (api, a.outCh);
    Vec3 listener { 0.f, 0.f, (float) a.listenerZ };
    setListener (api, listener);
    if (api.hasProbe())
    {
        at_perf::StatsC check {};
        if (api.getPerfStats (&check, (int) sizeof (check)) != 0 || check.version != 2)
        {
            std::fprintf (stderr, "Probe incompatible (StatsC version %u, attendu 2) : reconstruire moteur et bench avec le même At_PerfProbe.h\n", check.version);
            api.shutdown(); return 6;
        }
        api.setPerfOptions (a.perfOptions);
    }

    std::ofstream csv;
    if (! a.csv.empty())
    {
        std::ifstream probe (a.csv); const bool exists = probe.good(); probe.close();
        csv.open (a.csv, std::ios::app);
        if (! exists) writeCsvHeader (csv);
    }

    const int nCores = (int) std::max (1u, std::thread::hardware_concurrency());
    std::vector<int> uids;
    const auto tStart = steady_clock::now();
    auto nowS = [&] { return duration<double> (steady_clock::now() - tStart).count(); };

    std::printf ("\n%-6s %-6s %-7s %-8s %-9s %-9s %-8s %-9s %-7s %-6s %-6s\n",
                 "N", "t(s)", "block", "load%", "max%", "overrun", "denorm", "jitterus", "cpu", "migr", "log");

    for (int N : a.sources)
    {
        // ajoute les sources manquantes
        while ((int) uids.size() < N)
        {
            int uid = -1;
            if (api.addPlayer (&uid, true, true) != 0 || uid < 0) { std::fprintf (stderr, "addPlayer a échoué\n"); break; }
            const auto& file = a.files[uids.size() % a.files.size()];
            if (api.setPlayerFilePath (uid, file.c_str()) != 0) std::fprintf (stderr, "setPlayerFilePath(%s) a échoué\n", file.c_str());
            if (api.setPlayerRealTimeParameter) api.setPlayerRealTimeParameter (uid, 0.f, 1.f, 1.f, 0.5f);   // gain en dB
            sendSource (api, uid, sourceBase ((int) uids.size(), N, (float) a.sourceZ));
            api.startPlayer (uid);
            uids.push_back (uid);
        }
        // repositionne toutes les sources pour le palier N
        for (int i = 0; i < (int) uids.size(); ++i) sendSource (api, uids[i], sourceBase (i, N, (float) a.sourceZ));

        const double tEnd = nowS() + a.warmup + a.duration;
        const double tMeasure = nowS() + a.warmup;
        bool measuring = false;
        double nextUpdate = nowS(), nextReport = tMeasure;
        double cpu0 = processCpuSeconds(), wall0 = nowS();
        uint64_t log0 = g_logCount.load();
        uint64_t totOver = 0, totCb = 0, totDen = 0; double worstMax = 0, sumMean = 0; int nSamples = 0; uint32_t blk = 0; double fs = 0;

        while (nowS() < tEnd)
        {
            const double t = nowS();

            if (a.updateHz > 0 && t >= nextUpdate)
            {
                nextUpdate += 1.0 / a.updateHz;
                const double ph = 2.0 * 3.14159265358979 * 0.25 * t;
                for (int i = 0; i < (int) uids.size(); ++i)
                {
                    Vec3 p = sourceBase (i, N, (float) a.sourceZ);
                    if (a.motion == "orbit")       { p.x += 0.4f * (float) std::cos (ph + i); p.z += 0.4f * (float) std::sin (ph + i); }
                    else if (a.motion == "jitter") { p.x += 0.002f * (float) ((i * 7919 + (int) (t * 1000)) % 3 - 1); }
                    sendSource (api, uids[i], p);   // même en "static" : reproduit l'envoi par frame de Unity
                }
                if (a.motion == "listener")
                    setListener (api, { 0.8f * (float) std::sin (ph), 0.f, (float) a.listenerZ });
                else
                    setListener (api, listener);
            }

            if (! measuring && t >= tMeasure)
            {
                measuring = true;
                if (api.hasProbe()) api.resetPerfStats();
                cpu0 = processCpuSeconds(); wall0 = t; log0 = g_logCount.load();
                nextReport = t + 1.0;
            }

            if (measuring && t >= nextReport)
            {
                nextReport += 1.0;
                const double cpu1 = processCpuSeconds(), wall1 = nowS();
                const double cpuPct = 100.0 * (cpu1 - cpu0) / std::max (1e-6, wall1 - wall0);
                const uint64_t logs = g_logCount.load() - log0;
                cpu0 = cpu1; wall0 = wall1; log0 = g_logCount.load();

                at_perf::StatsC s {};
                if (api.hasProbe())
                {
                    api.getPerfStats (&s, (int) sizeof (s));
                    api.resetPerfStats();   // fenêtres d'une seconde
                }
                const double load = s.budgetUs > 0 ? 100.0 * s.meanUs / s.budgetUs : 0.0;
                const double maxL = s.budgetUs > 0 ? 100.0 * s.maxUs / s.budgetUs : 0.0;
                totOver += s.overruns; totCb += s.callbacks; totDen += s.denormalHits;
                worstMax = std::max (worstMax, maxL); sumMean += load; ++nSamples; blk = s.blockSize; fs = s.sampleRate;

                if (api.hasProbe())
                    std::printf ("%-6d %-6.0f %-7u %-8.1f %-9.1f %-9llu %-8llu %-9.0f %-7u %-6u %-6llu\n",
                                 N, t - tMeasure, s.blockSize, load, maxL, (unsigned long long) s.overruns,
                                 (unsigned long long) s.denormalHits, s.maxIntervalJitterUs, s.lastCpu, s.cpuMigrations,
                                 (unsigned long long) s.logCallsFromAudio);
                else
                    std::printf ("%-6d %-6.0f CPU process = %.1f %% d'un cœur (%.1f %% machine), logs/s = %llu\n",
                                 N, t - tMeasure, cpuPct, cpuPct / nCores, (unsigned long long) logs);

                if (csv.is_open())
                {
                    csv << a.label << ",\"" << a.device << "\"," << a.outCh << "," << a.buffer << "," << a.motion << ","
                        << a.updateHz << "," << a.perfOptions << "," << a.affinity << "," << a.priority << "," << a.noThrottle << ","
                        << a.prefilter << "," << a.wfsGain << "," << a.minMax << "," << a.mask << "," << (a.downmix ? a.render : std::string ("none")) << ","
                        << N << "," << (t - tMeasure) << "," << s.blockSize << "," << s.sampleRate << "," << s.budgetUs << ","
                        << s.callbacks << "," << s.overruns << "," << s.nearOverruns << "," << s.meanUs << "," << s.maxUs << ","
                        << s.maxIntervalUs << "," << s.maxIntervalJitterUs << "," << s.denormalHits << "," << s.ftzDazOffCallbacks << ","
                        << s.lastCpu << "," << s.cpuMigrations << "," << s.threadPriority << "," << s.logCallsFromAudio << ","
                        << logs << "," << cpuPct;
                    for (int k = 0; k < (int) at_perf::Section::Count; ++k)
                        csv << "," << s.sectionMeanUs[k] << "," << s.sectionMaxUs[k];
                    csv << "\n";
                    csv.flush();
                }
            }
            std::this_thread::sleep_for (milliseconds (1));
        }

        if (api.hasProbe())
            std::printf ("== N=%d : bloc réel %u @ %.0f Hz | charge moy %.1f %% | pire pic %.1f %% | overruns %llu / %llu callbacks | dénormaux %llu\n\n",
                         N, blk, fs, nSamples ? sumMean / nSamples : 0.0, worstMax,
                         (unsigned long long) totOver, (unsigned long long) totCb, (unsigned long long) totDen);

        if (a.stopOnFirstGlitchStep && totOver > 0) { std::printf ("Overruns détectés : arrêt de la montée.\n"); break; }
    }

    for (int uid : uids) { api.stopPlayer (uid); api.removePlayer (uid); }
    api.shutdown();
  #if defined(_WIN32)
    if (a.timerRes) timeEndPeriod (1);
  #endif
    return 0;
}
