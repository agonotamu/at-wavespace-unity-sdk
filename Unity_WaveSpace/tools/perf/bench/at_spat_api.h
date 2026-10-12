// at_spat_api.h — chargement dynamique de la DLL du moteur (at_wavespace_engine) pour le bench.
//
// Signatures vérifiées contre _at_wavespace_engine/Source/PluginInterface.h (oct. 2026) :
// exports préfixés AT_WS_, convention __stdcall sous Windows, return codes 0 = OK / 1 = erreur.
// Attention : les gains (setMasterGain, gain de setPlayerRealTimeParameter) sont en dB.
// x64 uniquement : en Win32 les exports __stdcall sont décorés (_AT_WS_xxx@n).
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

#if defined(_WIN32)
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
  using LibHandle = HMODULE;
  #define AT_CALL __stdcall      // CALL_CONV de PluginInterface.h
#else
  #include <dlfcn.h>
  using LibHandle = void*;
  #define AT_CALL
#endif

// Doit rester identique à at_perf::StatsC côté moteur (_at_wavespace_engine/Source/At_PerfProbe.h)
#include "../engine/At_PerfProbe.h"

extern "C"
{
    // Le callback de log n'a pas de convention explicite dans PluginInterface.h (cdecl par défaut).
    typedef void (*AT_LogCallback) (const char* message);

    typedef int  (AT_CALL *Fn_initialize) ();
    typedef int  (AT_CALL *Fn_shutdown) ();
    typedef int  (AT_CALL *Fn_setup) (const char* deviceName, int inputCh, int outputCh, int bufferSize, bool isBinaural);
    typedef int  (AT_CALL *Fn_addPlayer) (int* uid, bool is3D, bool isLooping);
    typedef int  (AT_CALL *Fn_removePlayer) (int uid);
    typedef int  (AT_CALL *Fn_startPlayer) (int uid);
    typedef int  (AT_CALL *Fn_stopPlayer) (int uid);
    typedef int  (AT_CALL *Fn_setPlayerFilePath) (int uid, const char* path);
    typedef int  (AT_CALL *Fn_setPlayerTransform) (int uid, float* pos, float* rot, float* fwd);
    typedef int  (AT_CALL *Fn_setPlayerRealTimeParameter) (int uid, float gainDb, float speed, float atten, float minDist);
    typedef int  (AT_CALL *Fn_setListenerTransform) (float* pos, float* rot, float* fwd);
    typedef int  (AT_CALL *Fn_setVirtualSpeakerTransform) (float* pos, float* rot, float* fwd, int count);
    typedef int  (AT_CALL *Fn_setMasterGain) (float gainDb);
    typedef int  (AT_CALL *Fn_setIsSimpleBinauralSpat) (bool b);
    typedef int  (AT_CALL *Fn_setBool) (bool b);      // setIsPrefilterAllPlayers, setIsWfsGain, …
    typedef int  (AT_CALL *Fn_setFloat) (float v);    // setSecondarySourceSize, setMaxDistanceForDelay
    typedef void (AT_CALL *Fn_setLogCallback) (AT_LogCallback cb);
    typedef int  (AT_CALL *Fn_setInt) (int v);        // setBinauralRenderMode
    typedef void (AT_CALL *Fn_refreshDevices) (int includeASIO, int async);
    typedef int  (AT_CALL *Fn_getDeviceCount) ();
    typedef int  (AT_CALL *Fn_getCachedDeviceInfo) (int index, char* name256, char* type64, int* maxIn, int* maxOut);
    typedef int  (AT_CALL *Fn_waitForDeviceScan) (int timeoutMs);
    typedef int  (AT_CALL *Fn_getPlayerWfsDelay) (int uid, float* out, int size);

    // Ajoutés par engine/PATCH.md (présents seulement si la DLL est construite avec AT_SPAT_PERF_PROBE)
    typedef int  (AT_CALL *Fn_getPerfStats) (at_perf::StatsC* out, int sizeBytes);
    typedef int  (AT_CALL *Fn_resetPerfStats) ();
    typedef int  (AT_CALL *Fn_setPerfOptions) (unsigned int options);
}

struct AtSpatApi
{
    LibHandle lib = nullptr;

    Fn_initialize                   initialize = nullptr;
    Fn_shutdown                     shutdown = nullptr;
    Fn_setup                        setup = nullptr;
    Fn_addPlayer                    addPlayer = nullptr;
    Fn_removePlayer                 removePlayer = nullptr;
    Fn_startPlayer                  startPlayer = nullptr;
    Fn_stopPlayer                   stopPlayer = nullptr;
    Fn_setPlayerFilePath            setPlayerFilePath = nullptr;
    Fn_setPlayerTransform           setPlayerTransform = nullptr;
    Fn_setPlayerRealTimeParameter   setPlayerRealTimeParameter = nullptr;
    Fn_setListenerTransform         setListenerTransform = nullptr;
    Fn_setVirtualSpeakerTransform   setVirtualSpeakerTransform = nullptr;
    Fn_setMasterGain                setMasterGain = nullptr;
    Fn_setIsSimpleBinauralSpat      setIsSimpleBinauralSpat = nullptr;
    Fn_setBool                      setIsPrefilterAllPlayers = nullptr;
    Fn_setBool                      setIsWfsGain = nullptr;
    Fn_setBool                      setIsActiveSpeakersMinMax = nullptr;
    Fn_setBool                      enableAllPlayersSpeakerMask = nullptr;
    Fn_setFloat                     setSecondarySourceSize = nullptr;
    Fn_setFloat                     setMaxDistanceForDelay = nullptr;
    Fn_setLogCallback               setLogCallback = nullptr;
    Fn_setInt                       setBinauralRenderMode = nullptr;
    Fn_refreshDevices               refreshDevices = nullptr;
    Fn_getDeviceCount               getDeviceCount = nullptr;
    Fn_getCachedDeviceInfo          getCachedDeviceInfo = nullptr;
    Fn_waitForDeviceScan            waitForDeviceScan = nullptr;
    Fn_getPlayerWfsDelay            getPlayerWfsDelay = nullptr;

    Fn_getPerfStats                 getPerfStats = nullptr;
    Fn_resetPerfStats               resetPerfStats = nullptr;
    Fn_setPerfOptions               setPerfOptions = nullptr;

    bool hasProbe() const { return getPerfStats && resetPerfStats && setPerfOptions; }

    bool load (const std::string& path)
    {
      #if defined(_WIN32)
        lib = LoadLibraryA (path.c_str());
      #else
        lib = dlopen (path.c_str(), RTLD_NOW);
      #endif
        if (! lib) { std::fprintf (stderr, "Impossible de charger %s\n", path.c_str()); return false; }

        bool ok = true;
        ok &= req (initialize, "AT_WS_initialize");
        ok &= req (shutdown, "AT_WS_shutdown");
        ok &= req (setup, "AT_WS_setup");
        ok &= req (addPlayer, "AT_WS_addPlayer");
        ok &= req (removePlayer, "AT_WS_removePlayer");
        ok &= req (startPlayer, "AT_WS_startPlayer");
        ok &= req (stopPlayer, "AT_WS_stopPlayer");
        ok &= req (setPlayerFilePath, "AT_WS_setPlayerFilePath");
        ok &= req (setPlayerTransform, "AT_WS_setPlayerTransform");
        ok &= req (setListenerTransform, "AT_WS_setListenerTransform");
        ok &= req (setVirtualSpeakerTransform, "AT_WS_setVirtualSpeakerTransform");

        opt (setPlayerRealTimeParameter, "AT_WS_setPlayerRealTimeParameter");
        opt (setMasterGain, "AT_WS_setMasterGain");
        opt (setIsSimpleBinauralSpat, "AT_WS_setIsSimpleBinauralSpat");
        opt (setIsPrefilterAllPlayers, "AT_WS_setIsPrefilterAllPlayers");
        opt (setIsWfsGain, "AT_WS_setIsWfsGain");
        opt (setIsActiveSpeakersMinMax, "AT_WS_setIsActiveSpeakersMinMax");
        opt (enableAllPlayersSpeakerMask, "AT_WS_enableAllPlayersSpeakerMask");
        opt (setSecondarySourceSize, "AT_WS_setSecondarySourceSize");
        opt (setMaxDistanceForDelay, "AT_WS_setMaxDistanceForDelay");
        opt (setLogCallback, "AT_WS_setLogCallback");
        opt (setBinauralRenderMode, "AT_WS_setBinauralRenderMode");
        opt (refreshDevices, "AT_WS_refreshDevices");
        opt (getDeviceCount, "AT_WS_getDeviceCount");
        opt (getCachedDeviceInfo, "AT_WS_getCachedDeviceInfo");
        opt (waitForDeviceScan, "AT_WS_waitForDeviceScan");
        opt (getPlayerWfsDelay, "AT_WS_getPlayerWfsDelay");

        opt (getPerfStats, "AT_WS_getPerfStats");
        opt (resetPerfStats, "AT_WS_resetPerfStats");
        opt (setPerfOptions, "AT_WS_setPerfOptions");
        return ok;
    }

private:
    void* sym (const char* name)
    {
      #if defined(_WIN32)
        return reinterpret_cast<void*> (GetProcAddress (lib, name));
      #else
        return dlsym (lib, name);
      #endif
    }
    template <typename F> bool req (F& f, const char* name)
    {
        f = reinterpret_cast<F> (sym (name));
        if (! f) std::fprintf (stderr, "Symbole obligatoire manquant : %s\n", name);
        return f != nullptr;
    }
    template <typename F> void opt (F& f, const char* name)
    {
        f = reinterpret_cast<F> (sym (name));
    }
};
