/**
 * @file AT_AudioManager.cpp
 * @brief Implementation of the main audio manager
 * @author Antoine Gonot
 * @date 2026
 */

#include "AT_AudioManager.h"
#include "UnityLogger.h"
#include <cstring>
#include <chrono>
#include <map>
#include "AT_SpatConfig.h"

#if JUCE_WINDOWS
#include <windows.h>   // HKEY, RegOpenKeyExW, RegEnumKeyExW, RegGetValueW, RegCloseKey
#include <string>
#endif

namespace AT
{
    // ========================================================================
    // CONSTRUCTOR / DESTRUCTOR
    // ========================================================================

    AudioManager::AudioManager()
        : m_devicesCached(false)
        , m_deviceSampleRate(0)
        , m_isPlayerActive(false)
    {
        // Register ourselves as listener for the spatialization engine
        m_spatializationEngine.setListener(this);

        // IMPORTANT: Do NOT scan devices here in the constructor.
        // Console applications have no JUCE message loop, so async scanning
        // will crash with JUCE_ASSERT_MESSAGE_THREAD.
        //
        // Console apps MUST call refreshDevices() manually:
        //   refreshDevices(includeASIO, false)  — synchronous scan
        //   Never use async = true in console applications.

#ifndef AT_SPAT_CONSOLE_APP
        #if JUCE_WINDOWS
           refreshDevices(true, true);   // async with ASIO
        #else
           refreshDevices(false, true);  // async without ASIO
        #endif

        LOG("[AudioManager] Created. Call refreshDevices(includeASIO, false) to scan devices.");
#endif
    }

    AudioManager::~AudioManager()
    {
        // Signal scan thread to stop
        m_shouldStopScan = true;

        // Wait for scan thread to complete (with timeout)
        if (m_scanThread.joinable())
        {
            try
            {
                m_scanThread.join();
            }
            catch (...)
            {
                LOG_WARNING("[AudioManager] Exception while joining scan thread");
            }
        }

        // Clean audio shutdown
        stop();
    }

    // ========================================================================
    // AudioManagerListener INTERFACE
    // ========================================================================

    void AudioManager::prepareToPlay(int samplesPerBlock, double sampleRate)
    {
        m_deviceSampleRate = sampleRate;
    }

    void AudioManager::releaseResources()
    {
        // No-op — can be extended if needed
    }

    // ========================================================================
    // DEVICE ENUMERATION
    // ========================================================================

    // ── Helper: enumerate names from a single AudioIODeviceType ─────────────────
    static void appendDeviceNames(juce::AudioIODeviceType* type,
                                   const std::string&        typeName,
                                   std::vector<DeviceInfo>& out)
    {
        if (!type) return;
        try
        {
            type->scanForDevices();
            for (const auto& name : type->getDeviceNames())
            {
                DeviceInfo info;
                info.name              = name.toStdString();
                info.typeName          = typeName;
                info.maxInputChannels  = -1;   // lazy-loaded in filterUnavailableDevices
                info.maxOutputChannels = -1;
                LOG("[AudioManager]   + " << info.name << " (" << typeName << ")");
                out.push_back(std::move(info));   // log BEFORE moving (moved-from string is empty)
            }
        }
        catch (const std::exception& e) { LOG_WARNING("[AudioManager] " << typeName << ": " << e.what()); }
        catch (...)                      { LOG_WARNING("[AudioManager] " << typeName << ": unknown exception"); }
    }

    // ── Helper: build an AudioIODeviceType for a given type name ──────────────
    static std::unique_ptr<juce::AudioIODeviceType> makeDeviceType(const std::string& typeName)
    {
        std::unique_ptr<juce::AudioIODeviceType> t;
        #if JUCE_WINDOWS
        if (typeName == "Windows Audio")
            t.reset(juce::AudioIODeviceType::createAudioIODeviceType_WASAPI(
                        juce::WASAPIDeviceMode::shared));
        #if JUCE_ASIO
        else if (typeName == "ASIO")
            t.reset(juce::AudioIODeviceType::createAudioIODeviceType_ASIO());
        #endif
        #elif JUCE_MAC
        if (typeName == "CoreAudio")
            t.reset(juce::AudioIODeviceType::createAudioIODeviceType_CoreAudio());
        #endif
        return t;
    }

   #if JUCE_WINDOWS && JUCE_ASIO
    // ── Helper: check that an ASIO driver's DLL is present on disk ────────────
    // Registry-only check (no COM loading, no driver code executed), so it stays
    // as fast as the registry enumeration itself.  Each subkey of
    // HKLM\SOFTWARE\ASIO holds a "CLSID" value; the matching
    // HKCR\CLSID\{...}\InprocServer32 default value is the driver DLL path.
    // Filters out orphaned entries left behind by uninstalled drivers.
    // NOTE: this does NOT detect a driver whose hardware is unplugged — that
    // can only be known by loading the driver (done lazily in
    // getDetailedDeviceInfo() when the user selects the device).
    //
    // Deliberately conservative: an entry is rejected only when the evidence is
    // unambiguous (no CLSID, no COM registration, or an absolute DLL path that
    // does not exist).  Anything unusual (relative path, unreadable value) is
    // kept — hiding a working device is worse than listing a stale one.
    static bool asioDriverDllExists(HKEY asioKey, const wchar_t* driverName)
    {
        wchar_t clsid[64];
        DWORD   size = sizeof(clsid);
        if (RegGetValueW(asioKey, driverName, L"CLSID", RRF_RT_REG_SZ,
                         nullptr, clsid, &size) != ERROR_SUCCESS)
            return false;   // no CLSID → JUCE could not load it either

        const std::wstring serverKey = L"CLSID\\" + std::wstring(clsid) + L"\\InprocServer32";

        HKEY serverHKey;
        if (RegOpenKeyExW(HKEY_CLASSES_ROOT, serverKey.c_str(), 0, KEY_READ, &serverHKey) != ERROR_SUCCESS)
            return false;   // COM class not registered → orphaned ASIO entry

        // RRF_RT_REG_SZ also accepts REG_EXPAND_SZ and expands it (e.g. %SystemRoot%).
        wchar_t dllPath[MAX_PATH] = {};
        size = sizeof(dllPath);
        const LONG res = RegGetValueW(serverHKey, nullptr, nullptr, RRF_RT_REG_SZ,
                                      nullptr, dllPath, &size);
        RegCloseKey(serverHKey);

        if (res != ERROR_SUCCESS)
            return true;    // unreadable default value — keep it, resolved lazily

        const juce::String path(dllPath);
        if (!juce::File::isAbsolutePath(path))
            return true;    // bare DLL name resolved via the search path — keep it

        return juce::File(path).existsAsFile();
    }
   #endif

    void AudioManager::scanAndCacheDevices(bool includeASIO)
    {
        // No AudioDeviceManager is created here, so no MIDI listener is registered
        // and no setCurrentThreadAsMessageThread() is required.
        // Only device names are retrieved; channel counts are lazily filled by
        // filterUnavailableDevices() and getDetailedDeviceInfo().

        LOG("[AudioManager] scanAndCacheDevices (includeASIO="
            << (includeASIO ? "true" : "false") << ")");

        auto startTime = std::chrono::high_resolution_clock::now();

        std::lock_guard<std::mutex> lock(m_deviceCacheMutex);
        m_cachedDevices.clear();

        // ── WASAPI ──────────────────────────────────────────────────────────────
        #if JUCE_WINDOWS
        {
            std::unique_ptr<juce::AudioIODeviceType> wasapi(
                juce::AudioIODeviceType::createAudioIODeviceType_WASAPI(
                    juce::WASAPIDeviceMode::shared));
            LOG("[AudioManager] Scanning WASAPI devices...");
            appendDeviceNames(wasapi.get(), "Windows Audio", m_cachedDevices);
        }

        // ── ASIO — read driver names from the Windows Registry, no COM loading ─
        // RegOpenKeyEx / RegEnumKeyEx are instantaneous; ASIO COM objects are NOT
        // loaded here.  Channel counts remain at -1 and are resolved lazily by
        // getDetailedDeviceInfo() when the user selects an ASIO device.
        #if JUCE_ASIO
        if (includeASIO)
        {
            LOG("[AudioManager] Scanning ASIO drivers from registry...");
            // NOTE: the backslash MUST be escaped.  "SOFTWARE\ASIO" (single
            // backslash) is an unknown escape sequence: MSVC silently turns it
            // into "SOFTWAREASIO" (warning C4129) and no ASIO driver is ever found.
            HKEY asioKey;
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO",
                              0, KEY_READ, &asioKey) == ERROR_SUCCESS)
            {
                wchar_t driverName[256];
                DWORD   nameLen = 256;
                DWORD   index   = 0;
                while (RegEnumKeyExW(asioKey, index++,
                                     driverName, &nameLen,
                                     nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS)
                {
                    const std::string name = juce::String(driverName).toStdString();
                    nameLen = 256;   // reset for the next RegEnumKeyExW call

                    // Skip orphaned entries (driver uninstalled, DLL gone).
                    // Registry-only check — no driver is loaded here.
                    if (!asioDriverDllExists(asioKey, driverName))
                    {
                        LOG("[AudioManager]   - " << name << " (ASIO, driver not installed — skipped)");
                        continue;
                    }

                    DeviceInfo info;
                    info.name              = name;
                    info.typeName          = "ASIO";
                    info.maxInputChannels  = -1;   // resolved lazily (getDetailedDeviceInfo)
                    info.maxOutputChannels = -1;
                    m_cachedDevices.push_back(std::move(info));
                    LOG("[AudioManager]   + " << name << " (ASIO)");
                }
                RegCloseKey(asioKey);
            }
            else
            {
                LOG("[AudioManager] No ASIO key found in registry");
            }
        }
        #endif // JUCE_ASIO

        #elif JUCE_MAC
        // ── CoreAudio ───────────────────────────────────────────────────────────
        {
            std::unique_ptr<juce::AudioIODeviceType> coreAudio(
                juce::AudioIODeviceType::createAudioIODeviceType_CoreAudio());
            LOG("[AudioManager] Scanning CoreAudio devices...");
            appendDeviceNames(coreAudio.get(), "CoreAudio", m_cachedDevices);
        }
        #endif

        m_devicesCached      = true;
        m_devicesCachedAtomic = true;

        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::high_resolution_clock::now() - startTime).count();
        LOG("[AudioManager] Scan complete: " << m_cachedDevices.size()
            << " device(s) in " << ms << " ms");
    }

    void AudioManager::scanDevicesAsync(bool includeASIO)
    {
        if (m_scanThread.joinable())
            m_scanThread.join();

        m_devicesCached = false;
        m_devicesCachedAtomic = false;
        m_shouldStopScan = false;

        m_scanThread = std::thread([this, includeASIO]() {
            scanAndCacheDevices(includeASIO);
        });
    }

    int AudioManager::getCachedDeviceCount() const
    {
        std::lock_guard<std::mutex> lock(m_deviceCacheMutex);
        return static_cast<int>(m_cachedDevices.size());
    }

    DeviceInfo AudioManager::getCachedDeviceInfo(int index) const
    {
        std::lock_guard<std::mutex> lock(m_deviceCacheMutex);

        if (index >= 0 && index < static_cast<int>(m_cachedDevices.size()))
            return m_cachedDevices[index];

        return DeviceInfo();
    }

    void AudioManager::refreshDevices()
    {
        // Synchronous rescan used by setup().  Must include ASIO on Windows,
        // otherwise the cache is rebuilt without the ASIO entries right before
        // the device is opened.  Cost is unchanged: ASIO names come from the
        // registry, no driver is loaded.  macOS: CoreAudio only, as before.
       #if JUCE_WINDOWS && JUCE_ASIO
        scanAndCacheDevices(true);
       #else
        scanAndCacheDevices(false);
       #endif
    }

    bool AudioManager::isDeviceScanComplete() const
    {
        return m_devicesCachedAtomic.load();
    }

    bool AudioManager::waitForDeviceScan(int timeoutMs)
    {
        if (m_devicesCachedAtomic)
            return true;

        auto startTime = std::chrono::steady_clock::now();

        while (!m_devicesCachedAtomic)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));

            if (timeoutMs > 0)
            {
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - startTime);

                if (elapsed.count() >= timeoutMs)
                {
                    LOG_WARNING("[AudioManager] Device scan timeout after " << elapsed.count() << " ms");
                    return false;
                }
            }
        }

        return true;
    }

    void AudioManager::refreshDevices(bool includeASIO, bool async)
    {
        if (async)
        {
            LOG("[AudioManager] Starting async device scan...");
            scanDevicesAsync(includeASIO);
        }
        else
        {
            LOG("[AudioManager] Starting synchronous device scan...");
            scanAndCacheDevices(includeASIO);
        }
    }

    DeviceInfo AudioManager::getDetailedDeviceInfo(int index)
    {
        // Called lazily (once) when the user selects a device in the Inspector.
        // Creates only the AudioIODeviceType matching the cached entry — no full
        // AudioDeviceManager, no MIDI registration.
        // For ASIO this may take a few seconds (COM object loaded on first call).
        std::lock_guard<std::mutex> lock(m_deviceCacheMutex);

        if (index < 0 || index >= static_cast<int>(m_cachedDevices.size()))
        {
            LOG_WARNING("[AudioManager] Invalid device index: " << index);
            return DeviceInfo();
        }

        DeviceInfo& info = m_cachedDevices[index];

        if (info.maxInputChannels != -1 && info.maxOutputChannels != -1)
            return info;
        if (info.maxOutputChannels == 0)
            return info;

        LOG("[AudioManager] Querying details for: " << info.name
            << " (" << info.typeName << ")");

        auto deviceType = makeDeviceType(info.typeName);
        if (!deviceType)
        {
            LOG_WARNING("[AudioManager] No device type factory for: " << info.typeName);
            info.maxInputChannels  = 0;
            info.maxOutputChannels = 0;
            return info;
        }

        try
        {
            deviceType->scanForDevices();
            std::unique_ptr<juce::AudioIODevice> device(
                deviceType->createDevice(juce::String(info.name),
                                          juce::String(info.name)));
            if (device)
            {
                info.maxOutputChannels = device->getOutputChannelNames().size();
                info.maxInputChannels  = device->getInputChannelNames().size();
                LOG("[AudioManager] " << info.name << ": "
                    << info.maxOutputChannels << " out, "
                    << info.maxInputChannels  << " in");
            }
            else
            {
                info.maxOutputChannels = 0;
                info.maxInputChannels  = 0;
                LOG_WARNING("[AudioManager] Could not create device: " << info.name);
            }
        }
        catch (const std::exception& e)
        {
            LOG_ERROR("[AudioManager] getDetailedDeviceInfo: " << e.what());
            info.maxInputChannels  = 0;
            info.maxOutputChannels = 0;
        }
        catch (...)
        {
            LOG_ERROR("[AudioManager] getDetailedDeviceInfo: unknown exception");
            info.maxInputChannels  = 0;
            info.maxOutputChannels = 0;
        }

        return info;
    }

    void AudioManager::filterUnavailableDevices()
    {
        // Validates devices and resolves channel counts without creating a full
        // AudioDeviceManager.  Strategy per driver type:
        //
        //   WASAPI  — createDevice() is fast (~10 ms); channels resolved immediately.
        //   ASIO    — COM loading is slow; ASIO devices are left at maxOutputChannels = -1
        //             and their channel count is resolved lazily by getDetailedDeviceInfo()
        //             when the user selects them in the Inspector.
        //   CoreAudio — same fast path as WASAPI.

        std::lock_guard<std::mutex> lock(m_deviceCacheMutex);
        LOG("[AudioManager] Filtering unavailable devices...");

        int valid   = 0;
        int skipped = 0;
        int invalid = 0;

        // Cache one live AudioIODeviceType per type name to avoid repeated COM init.
        std::map<std::string, std::unique_ptr<juce::AudioIODeviceType>> typeCache;

        for (auto& info : m_cachedDevices)
        {
            if (info.maxOutputChannels != -1)
                continue;

            // ASIO devices are validated lazily — loading their COM object here would
            // negate the speed gain of the registry-based scan.
            if (info.typeName == "ASIO")
            {
                ++skipped;
                LOG("[AudioManager] ASIO '" << info.name << "' — deferred to lazy load");
                continue;
            }

            // Build (or reuse) the AudioIODeviceType for this driver family.
            auto& typePtr = typeCache[info.typeName];
            if (!typePtr)
            {
                typePtr = makeDeviceType(info.typeName);
                if (typePtr)
                    typePtr->scanForDevices();
            }

            if (!typePtr)
            {
                info.maxInputChannels  = 0;
                info.maxOutputChannels = 0;
                ++invalid;
                continue;
            }

            try
            {
                std::unique_ptr<juce::AudioIODevice> device(
                    typePtr->createDevice(juce::String(info.name),
                                           juce::String(info.name)));
                if (device)
                {
                    info.maxOutputChannels = device->getOutputChannelNames().size();
                    info.maxInputChannels  = device->getInputChannelNames().size();
                    ++valid;
                    LOG("[AudioManager] '" << info.name << "': "
                        << info.maxOutputChannels << " out, "
                        << info.maxInputChannels  << " in");
                }
                else
                {
                    info.maxOutputChannels = 0;
                    info.maxInputChannels  = 0;
                    ++invalid;
                    LOG_WARNING("[AudioManager] '" << info.name << "' unavailable");
                }
            }
            catch (const std::exception& e)
            {
                LOG_WARNING("[AudioManager] filterUnavailableDevices '"
                    << info.name << "': " << e.what());
                info.maxOutputChannels = 0;
                info.maxInputChannels  = 0;
                ++invalid;
            }
            catch (...)
            {
                info.maxOutputChannels = 0;
                info.maxInputChannels  = 0;
                ++invalid;
            }
        }

        LOG("[AudioManager] Filter done: " << valid << " valid, "
            << skipped << " ASIO deferred, " << invalid << " unavailable");
    }

    // ========================================================================
    // AUDIO FILE MANAGEMENT
    // ========================================================================

    AudioFileMetadata AudioManager::getAudioFileMetadata(const std::string& filepath)
    {
        AudioFileMetadata metadata;

        juce::AudioFormatManager formatManager;
        formatManager.registerBasicFormats();

        juce::File audioFile(filepath);

        if (!audioFile.existsAsFile())
        {
            juce::Logger::writeToLog("AudioManager::getAudioFileMetadata() - File not found: " + juce::String(filepath));
            return metadata;
        }

        std::unique_ptr<juce::AudioFormatReader> reader(
            formatManager.createReaderFor(audioFile));

        if (reader == nullptr)
        {
            juce::Logger::writeToLog("AudioManager::getAudioFileMetadata() - Unable to read file: " + juce::String(filepath));
            return metadata;
        }

        metadata.numChannels  = static_cast<int>(reader->numChannels);
        metadata.sampleRate   = reader->sampleRate;
        metadata.totalSamples = reader->lengthInSamples;

        if (metadata.sampleRate > 0.0)
            metadata.lengthSeconds = static_cast<double>(metadata.totalSamples) / metadata.sampleRate;

        metadata.valid = true;

        return metadata;
    }

    // ========================================================================
    // PLAYER MANAGEMENT
    // ========================================================================

    void AudioManager::addPlayer(int* uid, bool is3D, bool isLooping)
    {
        m_spatializationEngine.addPlayer(uid, is3D, isLooping);
    }

    bool AudioManager::setPlayerFilePath(int uid, const char* path)
    {
        if (path == nullptr)
            return false;

        juce::String jucePath(path);
        return m_spatializationEngine.setPlayerFilePath(uid, jucePath);
    }

    bool AudioManager::removePlayer(int uid)
    {
        return m_spatializationEngine.removePlayer(uid);
    }

    // ========================================================================
    // AUDIO DEVICE SETUP
    // ========================================================================

    bool AudioManager::setup(const std::string& deviceName,
        int numInputChannels,
        int numVirtualSpeakers,
        int bufferSize,
        bool isBinauralVirtualization)
    {
        // ── Wait for the async device scan to finish ─────────────────────────────
        // AudioManager's constructor starts an asynchronous device scan.  On Windows
        // the scan includes ASIO drivers, which hold exclusive COM/driver resources
        // while running.  If setup() is called before the scan thread completes,
        // SpatializationEngine::setup() → m_deviceManager.initialise() may fail
        // because the background thread still owns those resources.
        // Waiting here (up to 10 s) is safe: the call is cheap when the scan has
        // already finished (atomic check), and it eliminates the race on every platform.
        if (!m_devicesCachedAtomic.load())
        {
            LOG("AudioManager::setup - waiting for device scan to complete...");
            waitForDeviceScan(10000);
        }

        // Ensure Unity's main thread is the JUCE message thread — required both
        // because AudioDeviceManager::initialise() registers MIDI listeners
        // that need JUCE_ASSERT_MESSAGE_THREAD, AND (see below) because the
        // device rescan we're about to force must run on this same thread.
        // Must happen BEFORE the rescan below: CoreAudio (and audio device
        // enumeration generally) is not reliably thread-safe for scanning from
        // a background thread. scanDevicesAsync() (used for the Editor device
        // list / cache) runs on a background std::thread; the fresh scan that
        // m_deviceManager.initialise() performs internally, just below, runs on
        // whichever thread calls it. If that background-thread scan leaves
        // CoreAudio's internal HAL state in a way the next scan doesn't see
        // consistently, the device name match can silently fail and JUCE falls
        // back to the default device — no error, just the wrong device open.
        // Manually reselecting a device in the Editor works around this because
        // refreshDevices() runs scanAndCacheDevices() synchronously on the
        // calling (main) thread instead. Forcing that same synchronous,
        // main-thread rescan here — right before initialise(), on the same
        // thread initialise() itself will use — reproduces that same "healthy"
        // state on every setup() call, not just after a manual reselect.
        juce::MessageManager::getInstance()->setCurrentThreadAsMessageThread();

        // Remember the channel count already resolved for the requested device
        // (by filterUnavailableDevices() or getDetailedDeviceInfo()) — the rescan
        // below resets every entry to -1, which would otherwise disable the
        // channel-count guard further down.
        int knownMaxOutputChannels = -1;
        {
            std::lock_guard<std::mutex> lock(m_deviceCacheMutex);
            for (const auto& dev : m_cachedDevices)
            {
                if (deviceName.empty() || dev.name == deviceName)
                {
                    knownMaxOutputChannels = dev.maxOutputChannels;
                    break;
                }
            }
        }

        refreshDevices();

        // Re-inject the known channel count into the fresh cache entry.
        if (knownMaxOutputChannels > 0)
        {
            std::lock_guard<std::mutex> lock(m_deviceCacheMutex);
            for (auto& dev : m_cachedDevices)
            {
                if (deviceName.empty() || dev.name == deviceName)
                {
                    dev.maxOutputChannels = knownMaxOutputChannels;
                    break;
                }
            }
        }

        // Store the binaural flag and virtual speaker count before configuring the engine
        m_isBinauralVirtualization = isBinauralVirtualization;
        m_numVirtualSpeakers = numVirtualSpeakers;

        // In binaural mode the output is always stereo regardless of the number of
        // virtual speakers.  In direct WFS mode numVirtualSpeakers equals the number
        // of physical output channels opened on the audio device.
        int physicalOutputChannels = isBinauralVirtualization ? 2 : numVirtualSpeakers;

        // ── Channel count guard (non-binaural mode only) ─────────────────────────
        // Requesting more physical output channels than the device supports causes
        // JUCE to crash inside AudioDeviceManager::initialise() with no recoverable
        // exception.  The check uses the cached device list populated by
        // filterUnavailableDevices(); it is skipped when maxOutputChannels == -1
        // (not yet queried) to avoid blocking setup on a missing device entry.
        if (!isBinauralVirtualization)
        {
            std::lock_guard<std::mutex> lock(m_deviceCacheMutex);
            for (const auto& dev : m_cachedDevices)
            {
                // Match by name, or accept the first entry when deviceName is empty
                // (default device selection).
                if (deviceName.empty() || dev.name == deviceName)
                {
                    if (dev.maxOutputChannels > 0 &&
                        physicalOutputChannels > dev.maxOutputChannels)
                    {
                        LOG_ERROR("AudioManager::setup - requested " << physicalOutputChannels
                            << " output channel(s) exceeds device maximum ("
                            << dev.maxOutputChannels << ") for device '"
                            << dev.name << "'. "
                            << "Reduce numVirtualSpeakers or enable binaural virtualization.");
                        return false;
                    }
                    break;
                }
            }
        }

        LOG("AudioManager::setup - Virtual speakers: " << numVirtualSpeakers
            << ", Physical outputs: " << physicalOutputChannels
            << ", Binaural: " << (isBinauralVirtualization ? "YES" : "NO"));

        try
        {
            m_spatializationEngine.setup(
                juce::String(deviceName),
                numInputChannels,
                physicalOutputChannels,
                bufferSize,
                numVirtualSpeakers,
                isBinauralVirtualization
            );
        }
        catch (const std::exception& e)
        {
            LOG_ERROR("AudioManager::setup - SpatializationEngine::setup() threw: " << e.what());
            return false;
        }
        catch (...)
        {
            LOG_ERROR("AudioManager::setup - SpatializationEngine::setup() threw an unknown exception.");
            return false;
        }

        return true;
    }

    void AudioManager::startPlayer(int uid)
    {
        m_spatializationEngine.startPlayers(uid);
    }

    void AudioManager::stopPlayer(int uid)
    {
        m_spatializationEngine.stopPlayers(uid);
    }

    void AudioManager::startAllPlayers()
    {
        m_spatializationEngine.startAllPlayers();
    }

    void AudioManager::stopAllPlayers()
    {
        m_spatializationEngine.stopAllPayers();
    }

    void AudioManager::stop()
    {
        m_isPlayerActive = false;
        m_spatializationEngine.stopAllPayers();
        m_spatializationEngine.close();
        m_spatializationEngine.releaseResources();
    }

    // ========================================================================
    // GLOBAL SPATIALIZATION ENGINE SETTINGS
    // ========================================================================

    void AudioManager::setListenerTransform(float* position, float* rotation, float* forward)
    {
        m_spatializationEngine.setListenerTransform(position, rotation, forward);
    }

    void AudioManager::setVirtualSpeakerTransform(float* positions, float* rotations, float* forwards, int virtualSpeakerCount)
    {
        m_spatializationEngine.setVirtualSpeakerTransform(positions, rotations, forwards, virtualSpeakerCount);
    }

    void AudioManager::setMaxDistanceForDelay(float maxDistance)
    {
        m_spatializationEngine.setMaxDistanceForDelay(maxDistance);
    }

    void AudioManager::setMasterGain(float masterGain)
    {
        m_spatializationEngine.setMasterGain(masterGain);
    }

    void AudioManager::setMakeupMasterGain(float makeupMasterGain)
    {
        m_spatializationEngine.setMakeupMasterGain(makeupMasterGain);
    }

    // ========================================================================
    // GLOBAL SPATIALIZATION ENGINE GETTERS
    // ========================================================================

    void AudioManager::getMixerOutputMeters(float* meters, int arraySize)
    {
        m_spatializationEngine.getMixerOutputMeters(meters, arraySize);
    }

    // ========================================================================
    // SPATIALIZATION ENGINE SETTINGS FOR PLAYERS
    // ========================================================================

    void AudioManager::setPlayerTransform(int uid, float* position, float* rotation, float* forward)
    {
        m_spatializationEngine.setPlayerTransform(uid, position, rotation, forward);
    }

    void AudioManager::setPlayerRealTimeParameter(int uid, float gain, float playbackSpeed, float attenuation, float minDistance)
    {
        m_spatializationEngine.setPlayerRealTimeParameter(uid, gain, playbackSpeed, attenuation, minDistance);
    }

    void AudioManager::enableAllPlayersSpeakerMask(bool isWfsSpeakerMask)
    {
        m_spatializationEngine.enableAllPlayersSpeakerMask(isWfsSpeakerMask);
    }

    void AudioManager::setIsWfsGain(bool isWfsGain)
    {
        m_spatializationEngine.setIsWfsGain(isWfsGain);
    }

    // ========================================================================
    // SPATIALIZATION ENGINE GETTERS FOR PLAYERS
    // ========================================================================

    void AudioManager::getPlayerWfsDelay(int uid, float* delay, int arraySize)
    {
        m_spatializationEngine.getPlayerWfsDelay(uid, delay, arraySize);
    }

    void AudioManager::getPlayerWfsLinGain(int uid, float* linGain, int arraySize)
    {
        m_spatializationEngine.getPlayerWfsLinGain(uid, linGain, arraySize);
    }

    void AudioManager::getPlayerMeters(int uid, float* meters, int arraySize)
    {
        m_spatializationEngine.getPlayerMeters(uid, meters, arraySize);
    }

    void AudioManager::getPlayerSpeakerMask(int uid, float* speakerMask, int arraySize)
    {
        m_spatializationEngine.getPlayerSpeakerMask(uid, speakerMask, arraySize);
    }

    void AudioManager::getPlayerNumChannel(int uid, int* numChannel)
    {
        m_spatializationEngine.getPlayerNumChannel(uid, numChannel);
    }

    void AudioManager::setIsPrefilterAllPlayers(bool isPrefilter)
    {
        m_spatializationEngine.setIsPrefilterAllPlayers(isPrefilter);
    }

    // ========================================================================
    // ENGINE ACCESS
    // ========================================================================

    SpatializationEngine* AudioManager::getSpatializationEngine()
    {
        return &m_spatializationEngine;
    }


    void AudioManager::setIsActiveSpeakersMinMax(bool enabled)
    {
        m_spatializationEngine.setIsActiveSpeakersMinMax(enabled);
    }

    void AudioManager::setSecondarySourceSize(float secondarySourceSize)
    {
        m_spatializationEngine.setSecondarySourceSize(secondarySourceSize);
    }

}
