/**
 * @file AT_SpatializationEngine.cpp
 * @brief Implementation of the SpatializationEngine class
 * @author Antoine Gonot
 * @date 2025
 */

#include "AT_SpatializationEngine.h"
#include <stdexcept>
#include "UnityLogger.h"
#include "AT_SpatConfig.h"

namespace AT
{
    SpatializationEngine::SpatializationEngine()
        : m_audioThreadPool(std::max(1, juce::SystemStats::getNumCpus() - 1))
    {
        m_puPlayer = std::make_unique<juce::AudioSourcePlayer>();
        m_puPlayer->setSource(this);

        #ifdef AT_SPAT_ENABLE_MULTITHREADING
            m_useMultithreading.store(true);
            LOG("Multithreading ENABLED (" << m_audioThreadPool.getNumThreads() << " threads)");
        #else
            m_useMultithreading.store(false);
            LOG("Multithreading DISABLED");
        #endif

        m_isBinauralVirtualization = false;
        m_isSimpleBinauralSpat     = false;

        // Zero-fill static arrays to avoid undefined values before prepareToPlay()
        std::memset(m_metersArray,                 0, sizeof(m_metersArray));
        std::memset(m_virtualSpeakerPositionsFlat, 0, sizeof(m_virtualSpeakerPositionsFlat));
        std::memset(m_smoothedSpeakerPosX,         0, sizeof(m_smoothedSpeakerPosX));
        std::memset(m_smoothedSpeakerPosY,         0, sizeof(m_smoothedSpeakerPosY));
        std::memset(m_smoothedSpeakerPosZ,         0, sizeof(m_smoothedSpeakerPosZ));
        std::memset(m_smoothedSpeakerAzimuth,      0, sizeof(m_smoothedSpeakerAzimuth));
    }

    SpatializationEngine::~SpatializationEngine()
    {
        if (m_puPlayer)
        {
            m_deviceManager.removeAudioCallback(m_puPlayer.get());
            m_puPlayer->setSource(nullptr);
        }

        for (auto& spatPlayer : m_spatPlayers)
        {
            if (spatPlayer)
            {
                spatPlayer->stop();
                spatPlayer->releaseResources();
            }
        }
        m_spatPlayers.clear();

        if (m_hrtfCleanupThread.joinable())
            m_hrtfCleanupThread.join();  // wait end before JUCE is destroying
    }

    void SpatializationEngine::setListener(AudioManagerListener* listener)
    {
        this->m_pListener = listener;
    }

    void SpatializationEngine::setup(juce::String deviceName, int inputChannels, int outputChannels,
                                     int bufferSize, int numVirtualSpeakers, bool isBinauralVirtualization)
    {
        m_numOutputChannels        = outputChannels;
        m_numVirtualSpeakers       = numVirtualSpeakers;
        m_isBinauralVirtualization = isBinauralVirtualization;

        int requestedInputs     = (inputChannels  == -1) ? 0 : inputChannels;
        int requestedOutputs    = (outputChannels == -1) ? 2 : outputChannels;
        int requestedBufferSize = bufferSize;

        juce::String error = m_deviceManager.initialise(
            requestedInputs,
            requestedOutputs,
            nullptr,
            true,
            deviceName.isEmpty() ? juce::String() : deviceName
        );

        if (error.isNotEmpty())
            throw std::runtime_error("Failed to initialize audio device: " + error.toStdString());

        auto* currentDevice = m_deviceManager.getCurrentAudioDevice();
        if (currentDevice == nullptr)
            throw std::runtime_error("No audio device available after initialization");

        int actualInputChannels  = (inputChannels  == -1) ? currentDevice->getInputChannelNames().size()  : inputChannels;
        int actualOutputChannels = (outputChannels == -1) ? currentDevice->getOutputChannelNames().size() : outputChannels;

        if (actualInputChannels < 0 || actualOutputChannels < 0)
            throw std::runtime_error("Invalid channel count");

        int maxInputs  = currentDevice->getInputChannelNames().size();
        int maxOutputs = currentDevice->getOutputChannelNames().size();

        if (actualInputChannels > maxInputs)
            throw std::runtime_error("Requested input channels (" + juce::String(actualInputChannels).toStdString() +
                                   ") exceed device capability (" + juce::String(maxInputs).toStdString() + ")");

        if (actualOutputChannels > maxOutputs)
            throw std::runtime_error("Requested output channels (" + juce::String(actualOutputChannels).toStdString() +
                                   ") exceed device capability (" + juce::String(maxOutputs).toStdString() + ")");

        int actualBufferSize = currentDevice->getCurrentBufferSizeSamples();

        if (actualInputChannels != requestedInputs
            || actualOutputChannels != requestedOutputs
            || actualBufferSize != requestedBufferSize)
        {
            juce::AudioDeviceManager::AudioDeviceSetup currentSetup;
            m_deviceManager.getAudioDeviceSetup(currentSetup);

            juce::BigInteger inputChannelBits, outputChannelBits;
            for (int i = 0; i < actualInputChannels;  ++i) inputChannelBits.setBit(i);
            for (int i = 0; i < actualOutputChannels; ++i) outputChannelBits.setBit(i);

            currentSetup.inputChannels  = inputChannelBits;
            currentSetup.outputChannels = outputChannelBits;
            currentSetup.useDefaultInputChannels  = (actualInputChannels  == 0);
            currentSetup.useDefaultOutputChannels = (actualOutputChannels == 0);
            currentSetup.bufferSize = bufferSize;

            if (bufferSize > 0)
            {
                if (bufferSize < 32)   { LOG_WARNING("Buffer size " << bufferSize << " is too small, using 64");   bufferSize = 64;   }
                if (bufferSize > 8192) { LOG_WARNING("Buffer size " << bufferSize << " is too large, using 4096"); bufferSize = 4096; }

                bool isPowerOf2 = (bufferSize & (bufferSize - 1)) == 0;
                if (!isPowerOf2) LOG_WARNING("Buffer size " << bufferSize << " is not a power of 2, may cause issues");

                currentSetup.bufferSize = bufferSize;
                LOG("Setting buffer size to: " << bufferSize << " samples");
            }
            else
            {
                LOG("Using device default buffer size: " << currentSetup.bufferSize << " samples");
            }

            error = m_deviceManager.setAudioDeviceSetup(currentSetup, true);

            if (error.isNotEmpty())
                throw std::runtime_error("Failed to configure audio channels: " + error.toStdString());
        }

        // ====================================================================
        // HRTF PROCESSORS — created only when binaural mode is enabled.
        // Avoids ~3 072 heap allocations for N=1024 in non-binaural mode.
        // ====================================================================
        m_puHrtfProcessors.clear();
        if (m_isBinauralVirtualization)
        {
            m_puHrtfProcessors.reserve(numVirtualSpeakers);
            for (int i = 0; i < numVirtualSpeakers; ++i)
                m_puHrtfProcessors.push_back(std::make_unique<HRTFProcessor>());

            LOG("Created " << numVirtualSpeakers << " HRTF processors (binaural mode ON)");
        }
        else
        {
            LOG("HRTF processors NOT created (binaural mode OFF)");
        }

        LOG("=== SETUP ===");
        LOG("  numVirtualSpeakers: " << numVirtualSpeakers);
        LOG("  m_numOutputChannels: " << m_numOutputChannels);
        LOG("  Binaural mode: " << (m_isBinauralVirtualization ? "YES" : "NO"));
        LOG("=============");

        // Default circular arrangement for virtual speaker positions
        const int safeN = std::min(numVirtualSpeakers, MAX_VIRTUAL_SPEAKERS);
        for (int i = 0; i < safeN; ++i)
        {
            float angle = (i * 2.0f * juce::MathConstants<float>::pi) / safeN;
            m_virtualSpeakerPositionsFlat[i * 3 + 0] = std::sin(angle);  // X
            m_virtualSpeakerPositionsFlat[i * 3 + 1] = 0.0f;             // Y
            m_virtualSpeakerPositionsFlat[i * 3 + 2] = std::cos(angle);  // Z
        }

        LOG("Initialized " << safeN << " virtual speaker positions");
        LOG("Physical output channels: " << outputChannels
            << (m_isBinauralVirtualization ? " (BINAURAL MODE)" : ""));

        m_deviceManager.addAudioCallback(m_puPlayer.get());
    }

    void SpatializationEngine::addPlayer(int* uid, bool is3D, bool isLooping)
    {
        auto spatPlayer = std::make_unique<AT::SpatPlayer>(m_numVirtualSpeakers, is3D, isLooping);

        if (m_sampleRate > 0 && m_samplesPerBlock > 0)
            spatPlayer->prepareToPlay(m_samplesPerBlock, m_sampleRate);

        if (spatPlayer->getSpatializer() != nullptr)
            spatPlayer->getSpatializer()->setSpatializationEngine(this);

        if (m_isBinauralVirtualization && is3D)
        {
            if (!m_puHrtfProcessors.empty() && m_puHrtfProcessors[0])
            {
                // Create a dedicated HRTFProcessor for this player (not shared)
                auto dedicatedProcessor = std::make_unique<HRTFProcessor>();

                // Prepare with the current audio parameters
                if (m_sampleRate > 0 && m_samplesPerBlock > 0)
                    dedicatedProcessor->prepare(m_sampleRate, m_samplesPerBlock);

                // Copy HRTF data from processor[0] — no file re-read required
                dedicatedProcessor->loadHRTFFromSharedReader(
                    m_puHrtfProcessors[0]->getSOFAReader()
                );

                // Transfer ownership to the player
                spatPlayer->initializeSimpleBinaural(std::move(dedicatedProcessor), m_sampleRate);
                spatPlayer->setIsSimpleBinauralSpat(m_isSimpleBinauralSpat);

                LOG("Player " << m_lastID << ": simple binaural initialized with dedicated HRTFProcessor");
            }
        }

        // Apply current global flags so the new player starts with the correct state,
        // regardless of when the global setters were called relative to addPlayer().
        if (spatPlayer->getSpatializer() != nullptr)
        {
            spatPlayer->getSpatializer()->enablePlayerSpeakerMask(m_globalIsWfsSpeakerMask);
            spatPlayer->getSpatializer()->setIsWfsGain(m_globalIsWfsGain);
            spatPlayer->getSpatializer()->setIsActiveSpeakersMinMax(m_globalIsActiveSpeakersMinMax);
            spatPlayer->getSpatializer()->setSecondarySourceSize(m_globalSecondarySourceSize);
        }
        spatPlayer->setIsPrefilter(m_globalIsPrefilter);

        m_lastID++;
        spatPlayer->setUID(m_lastID);
        *uid = m_lastID;

        {
            const juce::SpinLock::ScopedLockType lock(m_playerListLock);
            m_spatPlayers.push_back(std::move(spatPlayer));
        }
        LOG("Player added with UID: " << m_lastID);
    }

    bool SpatializationEngine::setPlayerFilePath(int uid, juce::String path)
    {
        for (auto& spatPlayer : m_spatPlayers)
        {
            if (spatPlayer && spatPlayer->getUID() == uid)
                return spatPlayer->setFilePath(path);
        }
        return false;
    }

    bool SpatializationEngine::removePlayer(int uid)
    {
        // Locate the raw pointer without extracting from the list:
        // stopWithFade() must be called while the player is still in m_spatPlayers
        // so that the audio thread keeps calling updateForNextBlock() during the fade.
        SpatPlayer* rawPtr = nullptr;
        {
            const juce::SpinLock::ScopedLockType lock(m_playerListLock);
            for (auto& p : m_spatPlayers)
            {
                if (p && p->getUID() == uid)
                {
                    rawPtr = p.get();
                    break;
                }
            }
        }

        if (!rawPtr)
            return false;

        // Apply a click-free fade-out at the output stage (post delay-line / convolver),
        // then remove the player from the list once it is silent.
        rawPtr->stopWithFade();
        rawPtr->releaseResources();

        // Extract from the list under SpinLock.
        std::unique_ptr<SpatPlayer> playerToDefer;
        {
            const juce::SpinLock::ScopedLockType lock(m_playerListLock);
            for (auto it = m_spatPlayers.begin(); it != m_spatPlayers.end(); ++it)
            {
                if (*it && (*it)->getUID() == uid)
                {
                    playerToDefer = std::move(*it);
                    m_spatPlayers.erase(it);
                    break;
                }
            }
        }

        // Deposit in graveyard — destructor may be slow (DelayLine joins internal thread),
        // so defer destruction to the start of the next audio block.
        if (playerToDefer)
        {
            const juce::SpinLock::ScopedLockType lock(m_playerListLock);
            m_playersGraveyard.push_back(std::move(playerToDefer));
        }

        LOG("Player removed: UID " << uid << " (deferred deletion)");
        return true;
    }

    void SpatializationEngine::startPlayers(int uid)
    {
        for (auto& spatPlayer : m_spatPlayers)
        {
            if (spatPlayer && spatPlayer->getUID() == uid)
            {
                spatPlayer->start();
                break;
            }
        }
    }

    void SpatializationEngine::stopPlayers(int uid)
    {
        for (auto& spatPlayer : m_spatPlayers)
        {
            if (spatPlayer && spatPlayer->getUID() == uid)
            {
                // stopWithFade() applies a one-block linear ramp at the OUTPUT of
                // processAndAdd() (post delay-line for WFS, post-convolver for binaural),
                // which is the only correct fade point to avoid clicks.
                spatPlayer->stopWithFade();
                break;
            }
        }
    }

    void SpatializationEngine::startAllPlayers()
    {
        for (auto& spatPlayer : m_spatPlayers)
        {
            if (spatPlayer && !spatPlayer->isPlaying())
                spatPlayer->start();
        }
    }

    void SpatializationEngine::stopAllPayers()
    {
        for (auto& spatPlayer : m_spatPlayers)
        {
            if (spatPlayer)
                spatPlayer->stop();
        }
    }

    void SpatializationEngine::initializeGlobalSmoothers()
    {
        m_listenerPosXSmoother.reset(m_sampleRate, GLOBAL_SMOOTHING_TIME_SECONDS);
        m_listenerPosYSmoother.reset(m_sampleRate, GLOBAL_SMOOTHING_TIME_SECONDS);
        m_listenerPosZSmoother.reset(m_sampleRate, GLOBAL_SMOOTHING_TIME_SECONDS);
        m_listenerForwardXSmoother.reset(m_sampleRate, GLOBAL_SMOOTHING_TIME_SECONDS);
        m_listenerForwardYSmoother.reset(m_sampleRate, GLOBAL_SMOOTHING_TIME_SECONDS);
        m_listenerForwardZSmoother.reset(m_sampleRate, GLOBAL_SMOOTHING_TIME_SECONDS);

        m_listenerPosXSmoother.setCurrentAndTargetValue(m_listenerPosX);
        m_listenerPosYSmoother.setCurrentAndTargetValue(m_listenerPosY);
        m_listenerPosZSmoother.setCurrentAndTargetValue(m_listenerPosZ);
        m_listenerForwardXSmoother.setCurrentAndTargetValue(m_listenerForwardX);
        m_listenerForwardYSmoother.setCurrentAndTargetValue(m_listenerForwardY);
        m_listenerForwardZSmoother.setCurrentAndTargetValue(m_listenerForwardZ);

        m_smoothedListenerPosX     = m_listenerPosX;
        m_smoothedListenerPosY     = m_listenerPosY;
        m_smoothedListenerPosZ     = m_listenerPosZ;
        m_smoothedListenerForwardX = m_listenerForwardX;
        m_smoothedListenerForwardY = m_listenerForwardY;
        m_smoothedListenerForwardZ = m_listenerForwardZ;

        // Initialize per-speaker smoothers — iterate pre-allocated static arrays
        // (zero heap allocations)
        const int safeN = std::min(m_numVirtualSpeakers, MAX_VIRTUAL_SPEAKERS);

        for (int i = 0; i < safeN; ++i)
        {
            m_virtualSpeakerPosXSmoother[i].reset(m_sampleRate, GLOBAL_SMOOTHING_TIME_SECONDS);
            m_virtualSpeakerPosYSmoother[i].reset(m_sampleRate, GLOBAL_SMOOTHING_TIME_SECONDS);
            m_virtualSpeakerPosZSmoother[i].reset(m_sampleRate, GLOBAL_SMOOTHING_TIME_SECONDS);
            m_virtualSpeakerAzimuthSmoother[i].reset(m_sampleRate, GLOBAL_SMOOTHING_TIME_SECONDS);

            float initX = m_virtualSpeakerPositionsFlat[i * 3 + 0];
            float initY = m_virtualSpeakerPositionsFlat[i * 3 + 1];
            float initZ = m_virtualSpeakerPositionsFlat[i * 3 + 2];

            m_virtualSpeakerPosXSmoother[i].setCurrentAndTargetValue(initX);
            m_virtualSpeakerPosYSmoother[i].setCurrentAndTargetValue(initY);
            m_virtualSpeakerPosZSmoother[i].setCurrentAndTargetValue(initZ);

            m_smoothedSpeakerPosX[i] = initX;
            m_smoothedSpeakerPosY[i] = initY;
            m_smoothedSpeakerPosZ[i] = initZ;

            float initAzimuth = calculateTargetAzimuthForSpeaker(i);
            m_virtualSpeakerAzimuthSmoother[i].setCurrentAndTargetValue(initAzimuth);
            m_smoothedSpeakerAzimuth[i] = initAzimuth;
        }
    }

    void SpatializationEngine::prepareToPlay(int samplesPerBlock, double sampleRate)
    {
        LOG("Prepare to play with buffer size: " << samplesPerBlock);

        m_samplesPerBlock = samplesPerBlock;
        m_sampleRate      = sampleRate;

        if (m_pListener != nullptr)
            m_pListener->prepareToPlay(samplesPerBlock, sampleRate);

        for (auto& spatPlayer : m_spatPlayers)
        {
            if (spatPlayer)
                spatPlayer->prepareToPlay(samplesPerBlock, sampleRate);
        }

        // Prepare all HRTF processors (only present if binaural mode is on)
        for (auto& hrtfProcessor : m_puHrtfProcessors)
        {
            if (hrtfProcessor)
                hrtfProcessor->prepare(sampleRate, samplesPerBlock);
        }

        // Auto-load default HRTF once at initialization
        if (m_isBinauralVirtualization && !m_puHrtfProcessors.empty())
        {
            bool hrtfAlreadyLoaded = m_puHrtfProcessors[0] && m_puHrtfProcessors[0]->isHRTFLoaded();

            if (!hrtfAlreadyLoaded)
            {
                LOG("Auto-loading default HRTF at initialization...");
                bool success = loadDefaultHRTF();
                LOG(success ? "Default HRTF loaded successfully" : "ERROR: Failed to load default HRTF!");
            }
            else
            {
                LOG("HRTF already loaded (skipping auto-load)");
            }
        }

        m_binauralTemp.setSize(2, samplesPerBlock);
        m_binauralTemp.clear();

        // prepare nearfield correction for binaural virtualization of WFS
        m_nearFieldCorrection.prepare(sampleRate);
                
        if (m_isBinauralVirtualization && m_numVirtualSpeakers > 0)
        {
            m_wfsBuffer.setSize(m_numVirtualSpeakers, samplesPerBlock);
            m_wfsBuffer.clear();
            LOG("WFS buffer initialized: " << m_numVirtualSpeakers << " channels");
            
            const int numThreads = m_audioThreadPool.getNumThreads();
            m_hrtfThreadBuffers.resize(numThreads);
            for (auto& buf : m_hrtfThreadBuffers)
                buf.setSize(2, samplesPerBlock);
            LOG("Pre-allocated " << numThreads << " HRTF thread buffers");
        }
        
        
        
        const int safeOutCh = std::min(m_numOutputChannels, MAX_VIRTUAL_SPEAKERS);
        for (int i = 0; i < safeOutCh; i++)
            m_metersArray[i] = -90.0f;

        initializeGlobalSmoothers();

        m_transitionGain.reset(sampleRate, BINAURALMODE_SMOOTHING_TIME_SECONDS);
        m_transitionGain.setCurrentAndTargetValue(1.0f);
    }

    void SpatializationEngine::close()
    {
        // Join previous async cleanup if still running
        if (m_hrtfCleanupThread.joinable())
            m_hrtfCleanupThread.join();
        
        stopAllPayers();

        if (m_puPlayer)
        {
            // Remove audio callback first — no audio thread will access
            // m_puHrtfProcessors after this line.
            m_deviceManager.removeAudioCallback(m_puPlayer.get());
            m_puPlayer->setSource(nullptr);
        }

        m_deviceManager.closeAudioDevice();

        if (!m_puHrtfProcessors.empty())
        {
            m_hrtfCleanupThread = std::thread([procs = std::move(m_puHrtfProcessors)]() mutable
            {
                procs.clear();
            });
            // Ne pas détacher — sera joint au prochain appel de close() ou dans le destructeur
        }
    }

    void SpatializationEngine::releaseResources()
    {
        if (m_pListener != nullptr)
            m_pListener->releaseResources();

        if (!m_spatPlayers.empty())
        {
            for (auto& spatPlayer : m_spatPlayers)
            {
                if (spatPlayer)
                    spatPlayer->releaseResources();
            }
            m_spatPlayers.clear();
        }
    }

    void SpatializationEngine::updateGlobalParametersTarget()
    {
        m_listenerPosXSmoother.setTargetValue(m_listenerPosX);
        m_listenerPosYSmoother.setTargetValue(m_listenerPosY);
        m_listenerPosZSmoother.setTargetValue(m_listenerPosZ);
        m_listenerForwardXSmoother.setTargetValue(m_listenerForwardX);
        m_listenerForwardYSmoother.setTargetValue(m_listenerForwardY);
        m_listenerForwardZSmoother.setTargetValue(m_listenerForwardZ);

        const int safeN = std::min(m_numVirtualSpeakers, MAX_VIRTUAL_SPEAKERS);
        for (int i = 0; i < safeN; ++i)
        {
            m_virtualSpeakerPosXSmoother[i].setTargetValue(m_virtualSpeakerPositionsFlat[i * 3 + 0]);
            m_virtualSpeakerPosYSmoother[i].setTargetValue(m_virtualSpeakerPositionsFlat[i * 3 + 1]);
            m_virtualSpeakerPosZSmoother[i].setTargetValue(m_virtualSpeakerPositionsFlat[i * 3 + 2]);
        }
        // NOTE: Azimuth is calculated in advanceGlobalSmoothers() from smoothed positions.
    }

    void SpatializationEngine::advanceGlobalSmoothers()
    {
        m_smoothedListenerPosX     = m_listenerPosXSmoother.getNextValue();
        m_smoothedListenerPosY     = m_listenerPosYSmoother.getNextValue();
        m_smoothedListenerPosZ     = m_listenerPosZSmoother.getNextValue();
        m_smoothedListenerForwardX = m_listenerForwardXSmoother.getNextValue();
        m_smoothedListenerForwardY = m_listenerForwardYSmoother.getNextValue();
        m_smoothedListenerForwardZ = m_listenerForwardZSmoother.getNextValue();

        const int safeN = std::min(m_numVirtualSpeakers, MAX_VIRTUAL_SPEAKERS);
        for (int i = 0; i < safeN; ++i)
        {
            m_smoothedSpeakerPosX[i] = m_virtualSpeakerPosXSmoother[i].getNextValue();
            m_smoothedSpeakerPosY[i] = m_virtualSpeakerPosYSmoother[i].getNextValue();
            m_smoothedSpeakerPosZ[i] = m_virtualSpeakerPosZSmoother[i].getNextValue();
            // Advance the dedicated azimuth smoother — targets are set in the two
            // dirty sections below whenever the listener or speakers move.
            m_smoothedSpeakerAzimuth[i] = m_virtualSpeakerAzimuthSmoother[i].getNextValue();
        }
    }

    void SpatializationEngine::processPlayersWFS(const juce::AudioSourceChannelInfo& bufferToFill)
    {
        // m_playerSnapshot was populated at the start of getNextAudioBlock() under SpinLock.
        // Safe to iterate here without any lock.
        const int numPlayers = static_cast<int>(m_playerSnapshot.size());

        // Route 3D players:
        //   - binaural mode  → m_wfsBuffer (N virtual channels, fed into HRTF chain)
        //   - direct mode    → bufferToFill (physical output channels)
        //
        // Route 2D players:
        //   - ALWAYS → bufferToFill (physical stereo/multi output, bypass HRTF entirely).
        //   2D players are a simple channel-copy passthrough. Sending them through
        //   m_wfsBuffer would feed them into processBinauralVirtualization() and
        //   spatialize them from virtual speaker positions 0..N-1 — wrong by design.
        juce::AudioSourceChannelInfo wfsTargetInfo;

        if (m_isBinauralVirtualization)
        {
            m_wfsBuffer.clear();
            wfsTargetInfo.buffer      = &m_wfsBuffer;
            wfsTargetInfo.startSample = 0;
            wfsTargetInfo.numSamples  = bufferToFill.numSamples;
        }
        else
        {
            wfsTargetInfo = bufferToFill;
        }

        bool useMultithreading = m_useMultithreading.load(std::memory_order_acquire) &&
                                 numPlayers >= MIN_PLAYERS_FOR_THREADING;

        if (!useMultithreading)
        {
            for (auto* spatPlayer : m_playerSnapshot)
                spatPlayer->updateForNextBlock();

            for (auto* spatPlayer : m_playerSnapshot)
            {
                if (!spatPlayer->isPlaying() || !spatPlayer->hasFileLoaded())
                    continue;

                // 2D players in non-binaural mode: write directly to bufferToFill
                //   (physical output, N channels = m_numVirtualSpeakers). No HRTF.
                //
                // 2D players in binaural mode: bufferToFill is only 2 channels (physical
                //   stereo). wfsTargetInfo = m_wfsBuffer (N channels), so we route 2D
                //   players there too. Each channel of the 2D file maps directly to the
                //   corresponding virtual speaker slot — processBinauralVirtualization()
                //   then applies HRTF per slot exactly as it would for a 3D source.
                //
                // In non-binaural mode wfsTargetInfo == bufferToFill, so both paths
                // are identical and the ternary has no cost.
                spatPlayer->processAndAdd(wfsTargetInfo);
            }
        }
        else
        {
            // In multithreaded mode each player writes into its own scratch buffer.
            // For 3D players the scratch buffer must be as wide as the WFS buffer
            // (m_numVirtualSpeakers channels). For 2D players the scratch buffer
            // only needs to match the physical output channel count (2 or more).
            // We allocate the larger of the two so the same pool can be reused for
            // both player types without per-block allocation.
            const int tempBufferChannels = m_isBinauralVirtualization ?
                                           m_numVirtualSpeakers :
                                           bufferToFill.buffer->getNumChannels();

            if (m_playerBuffers.size() < static_cast<size_t>(numPlayers))
            {
                m_playerBuffers.clear();
                for (int i = 0; i < numPlayers; i++)
                {
                    m_playerBuffers.push_back(
                        std::make_unique<juce::AudioBuffer<float>>(
                            tempBufferChannels, bufferToFill.numSamples
                        )
                    );
                }
            }

            for (auto& buffer : m_playerBuffers)
                buffer->clear();

            m_playersProcessed.store(0, std::memory_order_release);

            for (int i = 0; i < numPlayers; i++)
            {
                m_audioThreadPool.addJob([this, i, &bufferToFill]()
                {
                    // m_playerSnapshot is immutable for the duration of this block.
                    AT::SpatPlayer* player = m_playerSnapshot[i];

                    player->updateForNextBlock();

                    if (!player->isPlaying() || !player->hasFileLoaded())
                    {
                        m_playersProcessed.fetch_add(1, std::memory_order_release);
                        return;
                    }

                    juce::AudioSourceChannelInfo tempInfo;
                    tempInfo.buffer      = m_playerBuffers[i].get();
                    tempInfo.startSample = 0;
                    tempInfo.numSamples  = bufferToFill.numSamples;

                    player->processAndAdd(tempInfo);
                    m_playersProcessed.fetch_add(1, std::memory_order_release);
                });
            }

            const int maxWaitIterations = 500000;
            int waitCount = 0;

            while (m_playersProcessed.load(std::memory_order_acquire) < numPlayers)
            {
                if (++waitCount > maxWaitIterations)
                {
                    jassertfalse;
                    LOG_ERROR("Multithreading timeout! Processing remaining players sequentially.");

                    for (int i = 0; i < numPlayers; i++)
                    {
                        if (m_playersProcessed.load(std::memory_order_acquire) >= i + 1)
                            continue;
                        AT::SpatPlayer* player = m_playerSnapshot[i];
                        player->updateForNextBlock();
                        if (player->isPlaying() && player->hasFileLoaded())
                        {
                            juce::AudioSourceChannelInfo tempInfo;
                            tempInfo.buffer      = m_playerBuffers[i].get();
                            tempInfo.startSample = 0;
                            tempInfo.numSamples  = bufferToFill.numSamples;
                            player->processAndAdd(tempInfo);
                        }
                    }
                    break;
                }
                juce::Thread::yield();
            }

            // Mix all scratch buffers into wfsTargetInfo.
            // Both 3D and 2D players target wfsTargetInfo (see single-threaded path
            // comment above): in binaural mode this is m_wfsBuffer (N channels);
            // in non-binaural mode wfsTargetInfo == bufferToFill, behaviour unchanged.
            for (int i = 0; i < numPlayers; i++)
            {
                const int numChannelsToMix = wfsTargetInfo.buffer->getNumChannels();
                for (int ch = 0; ch < numChannelsToMix; ch++)
                {
                    juce::FloatVectorOperations::add(
                        wfsTargetInfo.buffer->getWritePointer(ch, wfsTargetInfo.startSample),
                        m_playerBuffers[i]->getReadPointer(ch),
                        wfsTargetInfo.numSamples
                    );
                }
            }
        }

        // Normalisation applied to wfsTargetInfo (the virtual-speaker bus).
        // In binaural mode this is m_wfsBuffer; in non-binaural mode it is
        // bufferToFill. All players (2D and 3D) are in this bus, so the
        // normalisation is applied uniformly.
        if (m_numVirtualSpeakers > 1)
        {
            const float normGain = 1.0f / std::sqrt(static_cast<float>(m_numVirtualSpeakers));
            wfsTargetInfo.buffer->applyGain(
                wfsTargetInfo.startSample, wfsTargetInfo.numSamples, normGain
            );
        }
    }

    void SpatializationEngine::processBinauralVirtualization(const juce::AudioSourceChannelInfo& bufferToFill)
    {
        const int numSamples     = bufferToFill.numSamples;
        const int processorCount = static_cast<int>(m_puHrtfProcessors.size());

        if (processorCount != m_numVirtualSpeakers)
        {
            LOG_ERROR("Size mismatch in processBinauralVirtualization(): "
                << "m_puHrtfProcessors.size()=" << processorCount
                << ", m_numVirtualSpeakers=" << m_numVirtualSpeakers);
            bufferToFill.clearActiveBufferRegion();
            return;
        }

        // Fix A: advance smoothers so calculateTargetAzimuthForSpeaker() reads
        // smoothed (not raw) positions. Without this, moving virtual speakers or
        // the listener causes azimuth jumps → audible glitch/click.
        // processSimpleBinaural() already does this; this brings the WFS binaural
        // path to the same level.
        for (int i = 0; i < numSamples; ++i)
            advanceGlobalSmoothers();

        // ── HRTF bypass thresholds ───────────────────────────────────────────────
        //
        // HRTF_SKIP_THRESHOLD  : peak amplitude below which a channel is considered
        //                        "quiet". Lowered from 0.00001 (−100 dBFS) to 1e-7
        //                        (−140 dBFS) to avoid triggering on weak musical
        //                        signal that has been attenuated by WFS gain/mask/
        //                        normalisation factors.
        //
        // HRTF_HOLD_BLOCKS     : number of consecutive blocks a channel must stay
        //                        below HRTF_SKIP_THRESHOLD before the convolution
        //                        is actually skipped. At 48 kHz / 512 samples this
        //                        is ~50 ms — long enough for the convolution IR tail
        //                        (typically 128–512 samples) to drain silently,
        //                        preventing the abrupt cut (noise gate) that was
        //                        audible with the previous single-block threshold.
        //                        Once the channel becomes active again, the counter
        //                        resets to 0 immediately so there is no onset delay.
        static constexpr float HRTF_SKIP_THRESHOLD = 1e-7f;   // −140 dBFS
        static constexpr int   HRTF_HOLD_BLOCKS    = 5;        // ~50 ms at 512/48k

        m_binauralTemp.clear();

        #ifdef AT_SPAT_ENABLE_MULTITHREADING
            if (m_numVirtualSpeakers >= MIN_SPEAKERS_FOR_HRTF_THREADING
                && !m_hrtfThreadBuffers.empty())
            {
                const int numThreads = static_cast<int>(m_hrtfThreadBuffers.size());
                for (auto& buf : m_hrtfThreadBuffers)
                    buf.clear();

                const int speakersPerThread = (m_numVirtualSpeakers + numThreads - 1) / numThreads;
                m_hrtfProcessed.store(0, std::memory_order_release);

                for (int t = 0; t < numThreads; ++t)
                {
                    m_audioThreadPool.addJob([this, t, numSamples, speakersPerThread]()\
                    {
                        const int startCh = t * speakersPerThread;
                        const int endCh   = std::min(startCh + speakersPerThread, m_numVirtualSpeakers);

                        for (int ch = startCh; ch < endCh; ++ch)
                        {
                            if (!m_puHrtfProcessors[ch])
                                continue;

                            // Hold-counter bypass: skip only after HRTF_HOLD_BLOCKS
                            // consecutive quiet blocks so the convolver tail drains cleanly.
                            const float* wfsData = m_wfsBuffer.getReadPointer(ch);
                            float maxAbs = 0.0f;
                            for (int i = 0; i < numSamples; ++i)
                                if (std::abs(wfsData[i]) > maxAbs)
                                    maxAbs = std::abs(wfsData[i]);

                            if (maxAbs < HRTF_SKIP_THRESHOLD)
                            {
                                if (++m_hrtfChannelHold[ch] > HRTF_HOLD_BLOCKS)
                                    continue;   // truly silent: skip convolution
                                // still within hold period: fall through and process
                                // so the tail drains without an abrupt cut
                            }
                            else
                            {
                                m_hrtfChannelHold[ch] = 0;   // channel active: reset
                            }

                            float azimuth = m_smoothedSpeakerAzimuth[ch];

                            // Each thread accumulates into its own buffer — no race condition
                            m_puHrtfProcessors[ch]->processAndAccumulate(
                                m_hrtfThreadBuffers[t], m_wfsBuffer, ch, numSamples, azimuth, 0.0f
                            );
                        }
                        m_hrtfProcessed.fetch_add(1, std::memory_order_release);
                    });
                }

                // Spin-wait (same pattern as processPlayersWFS)
                int waitCount = 0;
                while (m_hrtfProcessed.load(std::memory_order_acquire) < numThreads)
                {
                    if (++waitCount > 500000) { jassertfalse; break; }
                    juce::Thread::yield();
                }

                // Sum thread buffers into m_binauralTemp
                for (int t = 0; t < numThreads; ++t)
                {
                    juce::FloatVectorOperations::add(
                        m_binauralTemp.getWritePointer(0),
                        m_hrtfThreadBuffers[t].getReadPointer(0), numSamples);
                    juce::FloatVectorOperations::add(
                        m_binauralTemp.getWritePointer(1),
                        m_hrtfThreadBuffers[t].getReadPointer(1), numSamples);
                }
            }
            else
        #endif
            {
                // Sequential fallback (< MIN_SPEAKERS_FOR_HRTF_THREADING or multithreading disabled)
                for (int ch = 0; ch < m_numVirtualSpeakers; ++ch)
                {
                    if (!m_puHrtfProcessors[ch])
                        continue;

                    // Hold-counter bypass (same logic as threaded path above)
                    const float* wfsData = m_wfsBuffer.getReadPointer(ch);
                    float maxAbs = 0.0f;
                    for (int i = 0; i < numSamples; ++i)
                        if (std::abs(wfsData[i]) > maxAbs)
                            maxAbs = std::abs(wfsData[i]);

                    if (maxAbs < HRTF_SKIP_THRESHOLD)
                    {
                        if (++m_hrtfChannelHold[ch] > HRTF_HOLD_BLOCKS)
                            continue;
                    }
                    else
                    {
                        m_hrtfChannelHold[ch] = 0;
                    }

                    float azimuth = m_smoothedSpeakerAzimuth[ch];
                    m_puHrtfProcessors[ch]->processAndAccumulate(
                        m_binauralTemp, m_wfsBuffer, ch, numSamples, azimuth, 0.0f
                    );
                }
            }

        
        
        // ── Near-field ILD correction ───────────────────────
        // Applied in-place on m_binauralTemp before copy-out.
        // processStereo() is a no-op when m_bypass=true (r_virtual >= r_ref
        // or nearly frontal source), so the atomic load is the only cost
        // when correction is disabled.
        if (m_isNearFieldCorrection.load(std::memory_order_relaxed))
        {
            m_nearFieldCorrection.processStereo(
                m_binauralTemp.getWritePointer(0),
                m_binauralTemp.getWritePointer(1),
                numSamples
            );
        }

        // copy() is correct here: bufferToFill was cleared at the start of
        // getNextAudioBlock() and all players (2D and 3D) write into m_wfsBuffer,
        // so bufferToFill contains only zeros at this point.
        juce::FloatVectorOperations::copy(
            bufferToFill.buffer->getWritePointer(0, bufferToFill.startSample),
            m_binauralTemp.getReadPointer(0), numSamples
        );
        juce::FloatVectorOperations::copy(
            bufferToFill.buffer->getWritePointer(1, bufferToFill.startSample),
            m_binauralTemp.getReadPointer(1), numSamples
        );
    }

    void SpatializationEngine::processSimpleBinaural(const juce::AudioSourceChannelInfo& bufferToFill)
    {
        const int numSamples = bufferToFill.numSamples;
        const int numPlayers = static_cast<int>(m_playerSnapshot.size());

        for (int sample = 0; sample < numSamples; ++sample)
            advanceGlobalSmoothers();

        for (int i = 0; i < numPlayers; ++i)
        {
            AT::SpatPlayer* spatPlayer = m_playerSnapshot[i];

            // updateForNextBlock() must be called even for stopped players so that
            // AudioTransportSource::getNextAudioBlock() can set its stopped flag and
            // unblock AudioTransportSource::stop() on the main thread.
            spatPlayer->updateForNextBlock();

            if (!spatPlayer->isPlaying() || !spatPlayer->hasFileLoaded())
                continue;

            spatPlayer->processAndAdd(bufferToFill);
        }
    }

    void SpatializationEngine::getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill)
    {
        // Step 1: Drain the graveyard — destroy players removed during the previous block
        // outside the SpinLock, so their destructors don't hold the lock.
        {
            std::vector<std::unique_ptr<AT::SpatPlayer>> toDelete;
            {
                const juce::SpinLock::ScopedLockType lock(m_playerListLock);
                toDelete = std::move(m_playersGraveyard);
            }
        }

        // Step 2: Take an atomic snapshot of m_spatPlayers (raw pointer copies only).
        {
            const juce::SpinLock::ScopedLockType lock(m_playerListLock);
            m_playerSnapshot.clear();
            m_playerSnapshot.reserve(m_spatPlayers.size());
            for (auto& p : m_spatPlayers)
                if (p) m_playerSnapshot.push_back(p.get());
        }

        // Step 0: Adopt staged position updates from main thread (audio thread only).
        // setTargetValue() and per-Spatializer writes all happen here, serialised
        // with getNextValue() which also runs on the audio thread — eliminates the
        // LinearSmoothedValue data race that causes glitches on any movement.
        if (m_listenerTransformDirty.load(std::memory_order_acquire))
        {
            float pos[3], rot[3], fwd[3];
            {
                const juce::SpinLock::ScopedLockType lock(m_positionLock);
                std::memcpy(pos, m_pendingListenerPosition, 3 * sizeof(float));
                std::memcpy(rot, m_pendingListenerRotation, 3 * sizeof(float));
                std::memcpy(fwd, m_pendingListenerForward,  3 * sizeof(float));
            }
            m_listenerTransformDirty.store(false, std::memory_order_release);

            m_listenerPosX     = pos[0]; m_listenerPosY     = pos[1]; m_listenerPosZ     = pos[2];
            m_listenerForwardX = fwd[0]; m_listenerForwardY = fwd[1]; m_listenerForwardZ = fwd[2];

            m_listenerPosXSmoother.setTargetValue(m_listenerPosX);
            m_listenerPosYSmoother.setTargetValue(m_listenerPosY);
            m_listenerPosZSmoother.setTargetValue(m_listenerPosZ);
            m_listenerForwardXSmoother.setTargetValue(m_listenerForwardX);
            m_listenerForwardYSmoother.setTargetValue(m_listenerForwardY);
            m_listenerForwardZSmoother.setTargetValue(m_listenerForwardZ);

            updateNearFieldCorrectionGeometry();

            // Update azimuth smoother targets from the NEW raw listener transform.
            // Uses raw values (not smoothed arrays) because smoothers haven't advanced yet.
            if (m_isBinauralVirtualization)
            {
                float fwdX = m_listenerForwardX, fwdZ = m_listenerForwardZ;
                float fwdLen = std::sqrt(fwdX * fwdX + fwdZ * fwdZ);
                if (fwdLen > 0.0001f) { fwdX /= fwdLen; fwdZ /= fwdLen; }
                else                  { fwdX = 0.f; fwdZ = 1.f; }
                const float fwdAngle = std::atan2(fwdX, fwdZ);
                const int safeN = std::min(m_numVirtualSpeakers, MAX_VIRTUAL_SPEAKERS);
                for (int i = 0; i < safeN; ++i)
                {
                    float dx = m_virtualSpeakerPositionsFlat[i * 3 + 0] - m_listenerPosX;
                    float dz = m_virtualSpeakerPositionsFlat[i * 3 + 2] - m_listenerPosZ;
                    float az = (std::atan2(dx, dz) - fwdAngle)
                               * 180.f / juce::MathConstants<float>::pi;
                    while (az >  180.f) az -= 360.f;
                    while (az < -180.f) az += 360.f;
                    m_virtualSpeakerAzimuthSmoother[i].setTargetValue(az);
                }
            }

            for (auto* p : m_playerSnapshot)
                if (p && p->getSpatializer())
                    p->getSpatializer()->setListenerTransform(pos, rot, fwd);
        }

        if (m_speakerTransformDirty.load(std::memory_order_acquire))
        {
            int count;
            {
                const juce::SpinLock::ScopedLockType lock(m_positionLock);
                count = m_pendingSpeakerCount;
                std::memcpy(m_adoptPosBuf, m_pendingSpeakerPositions, count * 3 * sizeof(float));
                std::memcpy(m_adoptRotBuf, m_pendingSpeakerRotations, count * 3 * sizeof(float));
                std::memcpy(m_adoptFwdBuf, m_pendingSpeakerForwards,  count * 3 * sizeof(float));
            }
            m_speakerTransformDirty.store(false, std::memory_order_release);

            std::memcpy(m_virtualSpeakerPositionsFlat, m_adoptPosBuf, count * 3 * sizeof(float));

            for (int i = 0; i < count; ++i)
            {
                m_virtualSpeakerPosXSmoother[i].setTargetValue(m_adoptPosBuf[i * 3 + 0]);
                m_virtualSpeakerPosYSmoother[i].setTargetValue(m_adoptPosBuf[i * 3 + 1]);
                m_virtualSpeakerPosZSmoother[i].setTargetValue(m_adoptPosBuf[i * 3 + 2]);
            }

            // Update azimuth smoother targets from the NEW raw speaker positions.
            // Uses raw values (not smoothed arrays) because smoothers haven't advanced yet.
            if (m_isBinauralVirtualization)
            {
                float fwdX = m_listenerForwardX, fwdZ = m_listenerForwardZ;
                float fwdLen = std::sqrt(fwdX * fwdX + fwdZ * fwdZ);
                if (fwdLen > 0.0001f) { fwdX /= fwdLen; fwdZ /= fwdLen; }
                else                  { fwdX = 0.f; fwdZ = 1.f; }
                const float fwdAngle = std::atan2(fwdX, fwdZ);
                const int safeN = std::min(count, MAX_VIRTUAL_SPEAKERS);
                for (int i = 0; i < safeN; ++i)
                {
                    float dx = m_adoptPosBuf[i * 3 + 0] - m_listenerPosX;
                    float dz = m_adoptPosBuf[i * 3 + 2] - m_listenerPosZ;
                    float az = (std::atan2(dx, dz) - fwdAngle)
                               * 180.f / juce::MathConstants<float>::pi;
                    while (az >  180.f) az -= 360.f;
                    while (az < -180.f) az += 360.f;
                    m_virtualSpeakerAzimuthSmoother[i].setTargetValue(az);
                }
            }

            for (auto* p : m_playerSnapshot)
                if (p && p->getSpatializer())
                    p->getSpatializer()->setVirtualSpeakerTransform(m_adoptPosBuf, m_adoptRotBuf, m_adoptFwdBuf, count);
        }

        // Step 3: Detect mode change request and start the fade-out
        if (m_pendingModeChange.load(std::memory_order_acquire) && !m_isFadingToMode)
        {
            if (m_warmupBlocksRemaining > 0)
            {
                // Warmup in progress — memorize the request, do not start a new transition
                m_hasPendingModeChangeDuringWarmup = true;
                m_pendingModeAfterWarmup = m_targetSimpleBinauralMode.load(std::memory_order_acquire);
                m_pendingModeChange.store(false, std::memory_order_release);
                LOG("Audio thread: mode change request memorized during warmup");
            }
            else
            {
                m_targetModeAfterFade = m_targetSimpleBinauralMode.load(std::memory_order_acquire);
                m_transitionGain.setTargetValue(0.0f);
                m_isFadingToMode = true;
                m_pendingModeChange.store(false, std::memory_order_release);

                if (m_targetModeAfterFade)
                {
                    for (auto* spatPlayer : m_playerSnapshot)
                        spatPlayer->preWarmBinaural();
                }

                LOG("Audio thread: starting fade-out before mode switch to "
                    << (m_targetModeAfterFade ? "Simple Binaural" : "WFS + Binaural"));
            }
        }

        bufferToFill.clearActiveBufferRegion();

        const int numPlayers = static_cast<int>(m_playerSnapshot.size());

        if (numPlayers == 0)
        {
            const int safeOutCh = std::min(m_numOutputChannels, MAX_VIRTUAL_SPEAKERS);
            for (int i = 0; i < safeOutCh; i++)
                m_metersArray[i] = -90.0f;
            return;
        }

        // Step 4: Audio pipeline
        if (m_isSimpleBinauralSpat)
        {
            processSimpleBinaural(bufferToFill);
        }
        else
        {
            processPlayersWFS(bufferToFill);

            if (m_isBinauralVirtualization && !m_puHrtfProcessors.empty())
                processBinauralVirtualization(bufferToFill);
        }

        // Step 5: Apply transition gain (fade-out / fade-in)
        if (m_isFadingToMode || m_transitionGain.isSmoothing() || m_warmupBlocksRemaining > 0)
        {
            // Pre-compute the gain ramp for this block (equal-power transition)
            const int numSamples = bufferToFill.numSamples;
            for (int i = 0; i < numSamples; ++i)
                m_transitionGainBuffer[i] = std::sin(m_transitionGain.getNextValue()
                                                     * juce::MathConstants<float>::halfPi);

            // Apply gain ramp to all channels
            for (int ch = 0; ch < bufferToFill.buffer->getNumChannels(); ++ch)
            {
                float* data = bufferToFill.buffer->getWritePointer(ch, bufferToFill.startSample);
                for (int i = 0; i < numSamples; ++i)
                    data[i] *= m_transitionGainBuffer[i];
            }

            // Gain reached 0 — switch mode and prepare fade-in
            if (m_isFadingToMode && !m_transitionGain.isSmoothing()
                && m_transitionGain.getCurrentValue() < 0.001f)
            {
                m_isSimpleBinauralSpat = m_targetModeAfterFade;

                if (m_targetModeAfterFade)
                {
                    // Switching from WFS to Simple Binaural:
                    // prepare() clears pending JUCE crossfades, then preWarmBinaural()
                    // reloads the correct IR from a clean state.
                    for (auto* spatPlayer : m_playerSnapshot)
                    {
                        if (spatPlayer->getOwnedHrtfProcessor())
                            spatPlayer->getOwnedHrtfProcessor()->prepare(m_sampleRate, m_samplesPerBlock);
                        spatPlayer->preWarmBinaural();
                    }
                }
                else
                {
                    // Switching from Simple Binaural to WFS:
                    // Reset WFS delay lines (contain stale data not pushed during binaural mode)
                    for (auto* spatPlayer : m_playerSnapshot)
                        if (spatPlayer->getSpatializer())
                            spatPlayer->getSpatializer()->resetDelayLine();

                    // Reset WFS virtualization HRTF processors
                    for (auto& hrtfProcessor : m_puHrtfProcessors)
                        if (hrtfProcessor) hrtfProcessor->reset();
                }

                for (auto* spatPlayer : m_playerSnapshot)
                    spatPlayer->setIsSimpleBinauralSpat(m_targetModeAfterFade);

                // Do not start fade-in here — wait for warmup to complete first
                m_warmupBlocksRemaining = WARMUP_BLOCKS;

                m_isFadingToMode = false;
                LOG("Audio thread: mode switched + pipeline reset, starting warmup before fade-in to "
                    << (m_targetModeAfterFade ? "Simple Binaural" : "WFS + Binaural"));
            }
        }

        // Warmup countdown — start fade-in only once the convolver has stabilized
        if (m_warmupBlocksRemaining > 0)
        {
            --m_warmupBlocksRemaining;
            if (m_warmupBlocksRemaining == 0)
            {
                if (m_hasPendingModeChangeDuringWarmup)
                {
                    // Skip fade-in — a new transition is queued
                    m_hasPendingModeChangeDuringWarmup = false;
                    m_targetSimpleBinauralMode.store(m_pendingModeAfterWarmup, std::memory_order_release);
                    m_pendingModeChange.store(true, std::memory_order_release);
                    LOG("Audio thread: replaying memorized mode change request (skipping fade-in)");
                }
                else
                {
                    // No pending mode change — start normal fade-in
                    m_transitionGain.setTargetValue(1.0f);
                    LOG("Audio thread: warmup done, starting fade-in to "
                        << (m_targetModeAfterFade ? "Simple Binaural" : "WFS + Binaural"));
                }
            }
        }

        // Step 6: Output meters
        if (bufferToFill.buffer != nullptr)
        {
            const int safeOutCh = std::min(m_numOutputChannels, MAX_VIRTUAL_SPEAKERS);
            for (int channel = 0; channel < safeOutCh
                                  && channel < bufferToFill.buffer->getNumChannels(); channel++)
            {
                float rmsSum = 0.0f;
                float* channelData = bufferToFill.buffer->getWritePointer(channel, bufferToFill.startSample);

                for (int i = 0; i < bufferToFill.numSamples; i++)
                {
                    channelData[i] *= powf(10.0f, (m_masterGain + m_makeupMasterGain) / 20.0f);
                    rmsSum += channelData[i] * channelData[i];
                }

                float rmsValue = sqrtf(rmsSum / bufferToFill.numSamples);
                m_metersArray[channel] = (rmsValue > 0.0f) ? 20.0f * log10f(rmsValue) : -90.0f;
            }
        }
    }

    // ============================================================================
    // GLOBAL SPATIALIZATION ENGINE SETTINGS
    // ============================================================================

    void SpatializationEngine::setListenerTransform(float* position, float* rotation, float* forward)
    {
        // Write into staging buffer under lock — audio thread adopts in step 0.
        // Eliminates the race between setTargetValue() (main thread) and
        // getNextValue() (audio thread) on juce::LinearSmoothedValue.
        {
            const juce::SpinLock::ScopedLockType lock(m_positionLock);
            if (position) std::memcpy(m_pendingListenerPosition, position, 3 * sizeof(float));
            if (rotation) std::memcpy(m_pendingListenerRotation, rotation, 3 * sizeof(float));
            if (forward)  std::memcpy(m_pendingListenerForward,  forward,  3 * sizeof(float));
        }
        m_listenerTransformDirty.store(true, std::memory_order_release);
    }

    void SpatializationEngine::setVirtualSpeakerTransform(float* positions, float* rotations, float* forwards, int virtualSpeakerCount)
    {
        // HRTF processor resize stays on main thread (memory allocation).
        if (virtualSpeakerCount != m_numVirtualSpeakers && m_isBinauralVirtualization)
        {
            LOG_WARNING("Virtual speaker count changed from " << m_numVirtualSpeakers
                << " to " << virtualSpeakerCount);

            m_puHrtfProcessors.clear();
            m_puHrtfProcessors.reserve(virtualSpeakerCount);
            for (int i = 0; i < virtualSpeakerCount; ++i)
                m_puHrtfProcessors.push_back(std::make_unique<HRTFProcessor>());

            if (m_samplesPerBlock > 0 && m_sampleRate > 0)
            {
                for (auto& hrtf : m_puHrtfProcessors)
                {
                    if (hrtf)
                        hrtf->prepare(m_sampleRate, m_samplesPerBlock);
                }
            }
            LOG("HRTF processors resized to: " << m_puHrtfProcessors.size());
        }

        m_numVirtualSpeakers = virtualSpeakerCount;

        const int safeCount = std::min(virtualSpeakerCount, MAX_VIRTUAL_SPEAKERS);
        {
            const juce::SpinLock::ScopedLockType lock(m_positionLock);
            m_pendingSpeakerCount = safeCount;
            if (positions) std::memcpy(m_pendingSpeakerPositions, positions, safeCount * 3 * sizeof(float));
            if (rotations) std::memcpy(m_pendingSpeakerRotations, rotations, safeCount * 3 * sizeof(float));
            if (forwards)  std::memcpy(m_pendingSpeakerForwards,  forwards,  safeCount * 3 * sizeof(float));
        }
        m_speakerTransformDirty.store(true, std::memory_order_release);
    }

    void SpatializationEngine::setMaxDistanceForDelay(float maxDistance)
    {
        if (!m_spatPlayers.empty())
        {
            for (auto& spatPlayer : m_spatPlayers)
            {
                if (spatPlayer && spatPlayer->getSpatializer() != nullptr)
                    spatPlayer->getSpatializer()->setMaxDistanceForDelay(maxDistance);
            }
        }
    }

    // ============================================================================
    // GLOBAL SPATIALIZATION ENGINE GETTERS
    // ============================================================================

    void SpatializationEngine::getMixerOutputMeters(float* meters, int arraySize)
    {
        if (meters != nullptr && arraySize == m_numOutputChannels)
        {
            for (int i = 0; i < arraySize; i++)
                meters[i] = m_metersArray[i];
        }
    }

    // ============================================================================
    // SPATIALIZATION ENGINE SETTINGS FOR PLAYERS
    // ============================================================================

    void SpatializationEngine::setPlayerTransform(int uid, float* position, float* rotation, float* forward)
    {
        for (auto& spatPlayer : m_spatPlayers)
        {
            if (spatPlayer && spatPlayer->getUID() == uid)
            {
                if (spatPlayer->getSpatializer() != nullptr)
                    spatPlayer->getSpatializer()->setPlayerTransform(position, rotation, forward);

                // Track source position for NFC geometry (last-updated-player heuristic).
                // Correct for single-player corpus sessions; for multi-player, uses the
                // most recently moved source.
                m_nfcSourcePosX = position[0];
                m_nfcSourcePosY = position[1];
                m_nfcSourcePosZ = position[2];
                updateNearFieldCorrectionGeometry();
                break;
            }
        }
    }

    void SpatializationEngine::setPlayerRealTimeParameter(int uid, float gain, float playbackSpeed, float attenuation, float minDistance)
    {
        for (auto& spatPlayer : m_spatPlayers)
        {
            if (spatPlayer && spatPlayer->getUID() == uid)
            {
                spatPlayer->setGain(gain);
                spatPlayer->setPlaybackSpeed(playbackSpeed);
                if (spatPlayer->getSpatializer() != nullptr)
                {
                    spatPlayer->getSpatializer()->setPlayerAttenuation(attenuation);
                    spatPlayer->getSpatializer()->setPlayerMinDistance(minDistance);
                }
                break;
            }
        }
    }

    void SpatializationEngine::enableAllPlayersSpeakerMask(bool isWfsSpeakerMask)
    {
        m_globalIsWfsSpeakerMask = isWfsSpeakerMask;
        for (auto& spatPlayer : m_spatPlayers)
        {
            if (spatPlayer && spatPlayer->getSpatializer() != nullptr)
                spatPlayer->getSpatializer()->enablePlayerSpeakerMask(isWfsSpeakerMask);
        }
    }

    void SpatializationEngine::setMasterGain(float masterGain)
    {
        m_masterGain = masterGain;
    }

    void SpatializationEngine::setMakeupMasterGain(float makeupMasterGain)
    {
        m_makeupMasterGain = makeupMasterGain;
    }

    void SpatializationEngine::setIsWfsGain(bool isWfsGain)
    {
        m_globalIsWfsGain = isWfsGain;
        for (auto& spatPlayer : m_spatPlayers)
        {
            if (spatPlayer && spatPlayer->getSpatializer() != nullptr)
                spatPlayer->getSpatializer()->setIsWfsGain(isWfsGain);
        }
    }

    void SpatializationEngine::setIsActiveSpeakersMinMax(bool enabled)
    {
        m_globalIsActiveSpeakersMinMax = enabled;
        for (auto& spatPlayer : m_spatPlayers)
        {
            if (spatPlayer && spatPlayer->getSpatializer() != nullptr)
                spatPlayer->getSpatializer()->setIsActiveSpeakersMinMax(enabled);
        }
    }

    void SpatializationEngine::setSecondarySourceSize(float secondarySourceSize)
    {
        m_globalSecondarySourceSize = juce::jmax(0.0f, secondarySourceSize);
        for (auto& spatPlayer : m_spatPlayers)
        {
            if (spatPlayer && spatPlayer->getSpatializer() != nullptr)
                spatPlayer->getSpatializer()->setSecondarySourceSize(m_globalSecondarySourceSize);
        }
    }

    // ============================================================================
    // SPATIALIZATION ENGINE GETTERS FOR PLAYERS
    // ============================================================================

    void SpatializationEngine::getPlayerWfsDelay(int uid, float* delay, int arraySize)
    {
        for (auto& spatPlayer : m_spatPlayers)
        {
            if (spatPlayer && spatPlayer->getUID() == uid)
            {
                if (spatPlayer->getSpatializer() != nullptr)
                    spatPlayer->getSpatializer()->getPlayerWfsDelay(delay, arraySize);
                break;
            }
        }
    }

    void SpatializationEngine::getPlayerWfsLinGain(int uid, float* linGain, int arraySize)
    {
        for (auto& spatPlayer : m_spatPlayers)
        {
            if (spatPlayer && spatPlayer->getUID() == uid)
            {
                if (spatPlayer->getSpatializer() != nullptr)
                    spatPlayer->getSpatializer()->getWfsLinGain(linGain, arraySize);
                break;
            }
        }
    }

    void SpatializationEngine::getPlayerMeters(int uid, float* meters, int arraySize)
    {
        for (auto& spatPlayer : m_spatPlayers)
        {
            if (spatPlayer && spatPlayer->getUID() == uid)
            {
                spatPlayer->getMeters(meters, arraySize);
                break;
            }
        }
    }

    void SpatializationEngine::getPlayerSpeakerMask(int uid, float* speakerMask, int arraySize)
    {
        for (auto& spatPlayer : m_spatPlayers)
        {
            if (spatPlayer && spatPlayer->getUID() == uid)
            {
                if (spatPlayer->getSpatializer() != nullptr)
                    spatPlayer->getSpatializer()->getPlayerSpeakerMask(speakerMask, arraySize);
                break;
            }
        }
    }

    void SpatializationEngine::getPlayerNumChannel(int uid, int* numChannel)
    {
        for (auto& spatPlayer : m_spatPlayers)
        {
            if (spatPlayer && spatPlayer->getUID() == uid)
            {
                spatPlayer->getNumChannel(numChannel);
                break;
            }
        }
    }

    void SpatializationEngine::setIsPrefilterAllPlayers(bool isPrefilter)
    {
        m_globalIsPrefilter = isPrefilter;
        for (auto& spatPlayer : m_spatPlayers)
        {
            if (spatPlayer)
                spatPlayer->setIsPrefilter(isPrefilter);
        }
    }

    // ============================================================================
    // SIMPLE BINAURAL SPATIALIZATION
    // ============================================================================

    bool SpatializationEngine::setIsSimpleBinauralSpat(bool isSimple)
    {
        if (isSimple && !m_isBinauralVirtualization)
        {
            LOG_ERROR("Cannot enable simple binaural mode: binaural virtualization is not enabled");
            LOG_ERROR("Simple binaural mode requires isBinauralVirtualization = true (set in setup)");
            return false;
        }

        LOG("Requesting mode change to: " << (isSimple ? "Simple Binaural" : "WFS + Binaural"));
        m_targetSimpleBinauralMode.store(isSimple, std::memory_order_release);
        m_pendingModeChange.store(true,            std::memory_order_release);
        LOG("Mode change request queued (will be applied in audio thread)");
        return true;
    }

    // ============================================================================
    // MULTITHREADING CONTROL
    // ============================================================================

    void SpatializationEngine::setMultithreadingEnabled(bool enabled)
    {
        m_useMultithreading.store(enabled, std::memory_order_release);
        if (enabled)
            LOG("Multithreading ENABLED - " << m_audioThreadPool.getNumThreads() << " worker threads");
        else
            LOG("Multithreading DISABLED - sequential processing");
    }

    bool SpatializationEngine::isMultithreadingEnabled() const
    {
        return m_useMultithreading.load(std::memory_order_acquire);
    }

    // ============================================================================
    // BINAURAL VIRTUALIZATION
    // ============================================================================

    float SpatializationEngine::calculateTargetAzimuthForSpeaker(int speakerIndex)
    {
        if (speakerIndex < 0 || speakerIndex >= m_numVirtualSpeakers
                             || speakerIndex >= MAX_VIRTUAL_SPEAKERS)
            return 0.0f;

        float speakerX = m_smoothedSpeakerPosX[speakerIndex];
        float speakerZ = m_smoothedSpeakerPosZ[speakerIndex];

        float listenerX = m_smoothedListenerPosX;
        float listenerZ = m_smoothedListenerPosZ;

        float dx = speakerX - listenerX;
        float dz = speakerZ - listenerZ;

        float forwardX = m_smoothedListenerForwardX;
        float forwardZ = m_smoothedListenerForwardZ;

        float forwardLen = std::sqrt(forwardX * forwardX + forwardZ * forwardZ);
        if (forwardLen < 0.0001f)
        {
            forwardX = 0.0f;
            forwardZ = 1.0f;
        }
        else
        {
            forwardX /= forwardLen;
            forwardZ /= forwardLen;
        }

        float angleToSpeaker = std::atan2(dx, dz);
        float forwardAngle   = std::atan2(forwardX, forwardZ);
        float azimuthRad     = angleToSpeaker - forwardAngle;
        float azimuthDeg     = azimuthRad * 180.0f / juce::MathConstants<float>::pi;

        while (azimuthDeg >  180.0f) azimuthDeg -= 360.0f;
        while (azimuthDeg < -180.0f) azimuthDeg += 360.0f;

        return azimuthDeg;
    }

    bool SpatializationEngine::loadHRTFFile(const std::string& filePath)
    {
        LOG("=== LOADING HRTF FILE ===");
        LOG("  File path: " << filePath);

        if (!m_isBinauralVirtualization)
        {
            LOG_ERROR("Binaural virtualization not enabled — cannot load HRTF");
            return false;
        }
        if (m_puHrtfProcessors.empty())
        {
            LOG_ERROR("No HRTF processors created — call setup() first");
            return false;
        }
        if (!m_puHrtfProcessors[0])
        {
            LOG_ERROR("First HRTF processor is null");
            return false;
        }

        // Parse the file once into processor[0]
        bool success = m_puHrtfProcessors[0]->loadHRTFFromFile(filePath);
        if (!success)
        {
            LOG_ERROR("Failed to load HRTF file — check file format and path");
            return false;
        }

        // Copy the already-parsed data to all other processors (in-memory copy, no file I/O)
        const SOFAReader& sharedReader = m_puHrtfProcessors[0]->getSOFAReader();
        for (size_t i = 1; i < m_puHrtfProcessors.size(); ++i)
        {
            if (m_puHrtfProcessors[i])
                m_puHrtfProcessors[i]->loadHRTFFromSharedReader(sharedReader);
        }

        LOG("HRTF loaded into all " << m_puHrtfProcessors.size() << " processors");
        LOG("========================");
        return true;
    }

    bool SpatializationEngine::loadDefaultHRTF()
    {
        LOG("=== LOADING DEFAULT HRTF ===");

        if (!m_isBinauralVirtualization)
        {
            LOG_ERROR("Binaural virtualization not enabled — cannot load HRTF");
            return false;
        }
        if (m_puHrtfProcessors.empty())
        {
            LOG_ERROR("No HRTF processors created — call setup() first");
            return false;
        }
        if (!m_puHrtfProcessors[0])
        {
            LOG_ERROR("First HRTF processor is null");
            return false;
        }

        // Generate default HRTF data once in processor[0]
        m_puHrtfProcessors[0]->loadDefaultHRTF();

        // Copy the generated data to all other processors (in-memory copy, no FFT re-init)
        const SOFAReader& sharedReader = m_puHrtfProcessors[0]->getSOFAReader();
        for (size_t i = 1; i < m_puHrtfProcessors.size(); ++i)
        {
            if (m_puHrtfProcessors[i])
                m_puHrtfProcessors[i]->loadHRTFFromSharedReader(sharedReader);
        }

        LOG("Default HRTF loaded into all " << m_puHrtfProcessors.size() << " processors");
        LOG("===========================");
        return true;
    }

    void SpatializationEngine::setIsBinauralMode(bool enabled)
    {
        m_isBinauralVirtualization = enabled;
    }

    void SpatializationEngine::setIsBinauralVirtualization(bool isBinauralVirtualization)
    {
        m_isBinauralVirtualization = isBinauralVirtualization;
    }

    float SpatializationEngine::getSmoothedAzimuthForSpeaker(int speakerIndex) const
    {
        if (speakerIndex >= 0 && speakerIndex < m_numVirtualSpeakers
                              && speakerIndex < MAX_VIRTUAL_SPEAKERS)
            return m_smoothedSpeakerAzimuth[speakerIndex];
        return 0.0f;
    }

    void SpatializationEngine::getSmoothedListenerPosition(float& outX, float& outY, float& outZ) const
    {
        outX = m_smoothedListenerPosX;
        outY = m_smoothedListenerPosY;
        outZ = m_smoothedListenerPosZ;
    }

    void SpatializationEngine::getSmoothedListenerForward(float& outX, float& outY, float& outZ) const
    {
        outX = m_smoothedListenerForwardX;
        outY = m_smoothedListenerForwardY;
        outZ = m_smoothedListenerForwardZ;
    }

    void SpatializationEngine::getSmoothedVirtualSpeakerPosition(int speakerIndex, float& outX, float& outY, float& outZ) const
    {
        if (speakerIndex >= 0 && speakerIndex < m_numVirtualSpeakers
                              && speakerIndex < MAX_VIRTUAL_SPEAKERS)
        {
            outX = m_smoothedSpeakerPosX[speakerIndex];
            outY = m_smoothedSpeakerPosY[speakerIndex];
            outZ = m_smoothedSpeakerPosZ[speakerIndex];
        }
        else
        {
            outX = outY = outZ = 0.0f;
        }
    }

    void SpatializationEngine::setIsNearFieldCorrection(bool enabled)
    {
        m_isNearFieldCorrection.store(enabled, std::memory_order_release);
        if (!enabled)
            m_nearFieldCorrection.reset();
        else
            updateNearFieldCorrectionGeometry(); // sync filter to current geometry
        LOG("Near-field correction: " << (enabled ? "ON" : "OFF"));
    }

    void SpatializationEngine::setHrtfTruncate(bool enabled)
    {
        const int maxLen = enabled ? 512 : 0;
        for (int i = 0; i < static_cast<int>(m_puHrtfProcessors.size()); ++i)
        {
            if (m_puHrtfProcessors[i])
            {
                m_puHrtfProcessors[i]->setMaxIRLength(maxLen);
                // Force IR reload on next block so the new length takes effect.
                m_puHrtfProcessors[i]->preloadIR(
                    m_smoothedSpeakerAzimuth[i], 0.0f);
            }
        }
        LOG("HRTF truncation: " << (enabled ? "ON (512 samples)" : "OFF (full IR)"));
    }

    void SpatializationEngine::setNearFieldCorrectionRRef(float rRef, float headRadius)
    {
        m_nfcRRef       = rRef;
        m_nfcHeadRadius = headRadius;
        updateNearFieldCorrectionGeometry();
        LOG("NF correction: r_ref=" << rRef << "m  head_radius=" << headRadius << "m");
    }

    void SpatializationEngine::updateNearFieldCorrectionGeometry()
    {
        if (!m_isNearFieldCorrection.load(std::memory_order_relaxed))
            return;

        // rVirtual: horizontal distance listener → source (XZ plane, Y=up ignored).
        const float dx = m_nfcSourcePosX - m_listenerPosX;
        const float dz = m_nfcSourcePosZ - m_listenerPosZ;
        const float rVirtual = std::sqrt(dx * dx + dz * dz);

        // Azimuth: same convention as calculateTargetAzimuthForSpeaker()
        //   atan2(x, z) — XZ plane, +right = positive degrees.
        const float angleToSource = std::atan2(dx, dz);
        const float forwardAngle  = std::atan2(m_listenerForwardX, m_listenerForwardZ);
        float azimuthDeg = (angleToSource - forwardAngle) * 180.0f / juce::MathConstants<float>::pi;
        while (azimuthDeg >  180.0f) azimuthDeg -= 360.0f;
        while (azimuthDeg < -180.0f) azimuthDeg += 360.0f;

        m_nearFieldCorrection.setParameters(rVirtual, m_nfcRRef, azimuthDeg, m_nfcHeadRadius);
        LOG("NF correction geometry: r=" << rVirtual << "m  az=" << azimuthDeg
            << "deg  r_ref=" << m_nfcRRef << "m");
    }

}  // namespace AT
