#pragma once

#include <JuceHeader.h>
#include "SOFAReader.h"
#include <atomic>

/**
 * @brief OPTIMIZED: Binaural spatialization processor using HRTF
 * 
 * PERFORMANCE IMPROVEMENTS:
 * - Pre-allocated buffers (NO allocations in audio thread)
 * - SIMD-optimized operations (FloatVectorOperations)
 * - Thread-safe atomics for position updates
 * - juce::dsp::Convolution stored as VALUE MEMBERS (not unique_ptr)
 *   → eliminates 2 heap allocations per HRTFProcessor instance
 *   → for N=1024 processors this saves 2 048 make_unique calls at startup
 */
class HRTFProcessor
{
public:
    HRTFProcessor();
    ~HRTFProcessor();
    
    /**
     * @brief Prepares the processor for audio playback
     * @param sampleRate Sample rate in Hz
     * @param samplesPerBlock Maximum number of samples per processing block
     */
    void prepare(double sampleRate, int samplesPerBlock);
    
    /**
     * @brief Resets internal state (clears convolution delay lines)
     */
    void reset();
    
    /**
     * @brief Loads HRTF data from a SOFA file
     */
    bool loadHRTF(const juce::File& sofaFile);
    
    /**
     * @brief Loads HRTF data from a text file (simplified SOFA format)
     */
    bool loadHRTFFromFile(const std::string& filePath);
    
    /**
     * @brief Loads default built-in HRTF (simple generic HRTF)
     */
    bool loadDefaultHRTF();

    /**
     * @brief Copies already-parsed SOFA data from another processor (no file I/O).
     *
     * Use this to initialise processors[1..N-1] after processor[0] has been
     * loaded with loadHRTF() or loadDefaultHRTF(). Avoids re-parsing the file
     * N times — performs an in-memory copy of IR vectors instead.
     *
     * @param sharedReader  The already-loaded SOFAReader from processor[0].
     */
    void loadHRTFFromSharedReader(const SOFAReader& sharedReader);
    
    /**
     * @brief Processes audio buffer with HRTF spatialization (without distance)
     */
    void process(juce::AudioBuffer<float>& buffer, float azimuth, float elevation);
    
    /**
     * @brief Processes audio buffer with HRTF spatialization (with distance)
     */
    void process(juce::AudioBuffer<float>& buffer, float azimuth, float elevation, float distance);
    
    /**
     * @brief OPTIMIZED: Processes audio and accumulates to output buffer (without distance)
     */
    void processAndAccumulate(juce::AudioBuffer<float>& outputBuffer,
                              const juce::AudioBuffer<float>& sourceBuffer,
                              int sourceChannel, int numSamples,
                              float azimuth, float elevation);
    
    /**
     * @brief OPTIMIZED: Processes audio and accumulates to output buffer (with distance)
     */
    void processAndAccumulate(juce::AudioBuffer<float>& outputBuffer,
                              const juce::AudioBuffer<float>& sourceBuffer,
                              int sourceChannel, int numSamples,
                              float azimuth, float elevation, float distance);
    
    void setAzimuth(float azimuthDegrees)    { currentAzimuth.store(azimuthDegrees); }
    void setElevation(float elevationDegrees){ currentElevation.store(elevationDegrees); }
    void setDistance(float distanceMeters)   { currentDistance.store(distanceMeters); }
    
    float getAzimuth()  const { return currentAzimuth.load(); }
    float getElevation()const { return currentElevation.load(); }
    float getDistance() const { return currentDistance.load(); }
    
    bool isHRTFLoaded()     const { return sofaReader.isLoaded(); }
    bool hasDistanceInfo()  const { return sofaReader.hasDistanceInfo(); }
    float getMinDistance()  const { return sofaReader.getMinDistance(); }
    float getMaxDistance()  const { return sofaReader.getMaxDistance(); }
    
    SOFAReader& getSOFAReader() { return sofaReader; }
    
    /**
     * @brief Pre-loads the IR for a given azimuth (async, call before fade-in)
     * Forces IR update with correct azimuth so the async load completes
     * before audio processing starts.
     */
    void preloadIR(float azimuth, float elevation = 0.0f);

    /**
     * @brief Sets the maximum IR length in samples used for convolution.
     * @param maxLength  0 = no limit (use full IR); 512 = truncate to 512 samples.
     *                   Applied on the next IR reload triggered by updateIRs().
     */
    void setMaxIRLength(int maxLength) { m_maxIRLength = maxLength; }
    
private:
    SOFAReader sofaReader;
    
    double currentSampleRate;
    int    maxBlockSize;
    
    std::atomic<float> currentAzimuth;
    std::atomic<float> currentElevation;
    std::atomic<float> currentDistance;
    float targetDistance;
    
    // =========================================================================
    // VALUE MEMBERS for convolvers  (§ 3.6 Optimisation pre-allocation)
    //
    // Replaced from:
    //   std::unique_ptr<juce::dsp::Convolution> leftConvolver;
    //   std::unique_ptr<juce::dsp::Convolution> rightConvolver;
    //
    // Storing as direct value members eliminates 2 heap allocations per
    // HRTFProcessor instance. For N=1024 processors this saves 2 048
    // make_unique calls during PlayMode startup.
    //
    // HRTFProcessor is always accessed via unique_ptr from SpatializationEngine,
    // so the object itself is never copied or moved — direct members are safe.
    // =========================================================================
    juce::dsp::Convolution leftConvolver;
    juce::dsp::Convolution rightConvolver;
    
    // Pre-allocated processing buffers (set in prepare(), never reallocated in audio thread)
    juce::AudioBuffer<float> monoBuffer;
    juce::AudioBuffer<float> tempLeftBuffer;
    juce::AudioBuffer<float> tempRightBuffer;
    
    void resampleIR(const std::vector<float>& input, std::vector<float>& output, double ratio);
    void updateIRs();
    
    std::atomic<bool> needsUpdate;

    // Fix E: IR reload guard — prevents loadImpulseResponse() storms during movement.
    // Audio thread calls updateIRs() every block; it returns immediately (no-op) if
    // the nearest SOFA index has not changed, and enforces a cooldown between reloads
    // so the JUCE Convolution crossfade never piles up.
    int m_lastIRIndex = -1;    // index currently loaded in the convolvers
    int m_pendingIRIndex = -1; // index we want next
    int m_irReloadCooldown = 0;
    static constexpr int IR_RELOAD_COOLDOWN_BLOCKS = 5;

    // Maximum IR length in samples (0 = no limit).
    // When > 0, IRs are truncated to this length before loadImpulseResponse().
    // Set via setMaxIRLength(); propagated by SpatializationEngine::setHrtfTruncate().
    int m_maxIRLength = 0;
};
