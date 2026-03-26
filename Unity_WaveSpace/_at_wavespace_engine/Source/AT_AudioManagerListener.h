/**
 * @file AT_AudioManagerListener.h
 * @brief Abstract interface for audio manager lifecycle callbacks
 * @author Antoine Gonot
 * @date 2025
 */

#pragma once

#include <JuceHeader.h>

namespace AT
{
    /**
     * @class AudioManagerListener
     * @brief Abstract base class defining the interface for audio preparation and resource management
     * 
     * This class provides pure virtual methods that must be implemented by derived classes
     * to handle audio stream preparation and resource cleanup operations.
     */
    class AudioManagerListener
    {
        protected:
            /**
             * @brief Protected default constructor
             */
            AudioManagerListener() = default;
            
            /**
             * @brief Protected default destructor
             */
            ~AudioManagerListener() = default;
            
        public:
            /**
             * @brief Prepares the audio system for playback
             * @param samplesPerBlock Number of samples processed in each audio callback block
             * @param sampleRate Sample rate in Hz (samples per second)
             */
            virtual void prepareToPlay(int samplesPerBlock, double sampleRate) = 0;
            
            /**
             * @brief Releases all allocated audio resources
             * 
             * Called when audio processing is stopped or the system needs to free resources
             */
            virtual void releaseResources() = 0;
    };
}
