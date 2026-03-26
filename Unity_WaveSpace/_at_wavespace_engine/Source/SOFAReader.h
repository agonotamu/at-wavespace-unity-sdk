#pragma once

#include <JuceHeader.h>
#include <vector>
#include <string>

/**
 * @brief Simplified SOFA reader for HRTF files
 * 
 * Supports only the SimpleFreeFieldHRIR format
 */
class SOFAReader
{
public:
    /**
     * @brief Constructor
     */
    SOFAReader();
    
    /**
     * @brief Destructor
     */
    ~SOFAReader();
    
    /**
     * @brief Loads a SOFA file
     * @param file JUCE File object pointing to the SOFA file
     * @return True if file was loaded successfully, false otherwise
     */
    bool loadFile(const juce::File& file);
    
    /**
     * @brief Retrieves impulse responses for a given position (azimuth, elevation, distance in degrees/meters)
     * @param azimuth Azimuth angle in degrees
     * @param elevation Elevation angle in degrees
     * @param leftIR Output vector for left ear impulse response
     * @param rightIR Output vector for right ear impulse response
     * @return True if IRs were retrieved successfully, false otherwise
     */
    bool getIRsForPosition(float azimuth, float elevation, 
                          std::vector<float>& leftIR, 
                          std::vector<float>& rightIR);
    
    /**
     * @brief Retrieves impulse responses with distance selection
     * @param azimuth Azimuth angle in degrees
     * @param elevation Elevation angle in degrees
     * @param distance Distance in meters
     * @param leftIR Output vector for left ear impulse response
     * @param rightIR Output vector for right ear impulse response
     * @return True if IRs were retrieved successfully, false otherwise
     */
    bool getIRsForPositionWithDistance(float azimuth, float elevation, float distance,
                                       std::vector<float>& leftIR,
                                       std::vector<float>& rightIR);
    
    /**
     * @brief Gets the length of impulse responses in samples
     * @return IR length in samples
     */
    int getIRLength() const { return irLength; }
    
    /**
     * @brief Gets the sample rate of the HRTF dataset
     * @return Sample rate in Hz
     */
    int getSampleRate() const { return sampleRate; }
    
    /**
     * @brief Gets the number of measurements in the dataset
     * @return Number of measurements
     */
    int getNumMeasurements() const { return numMeasurements; }
    
    /**
     * @brief Checks if HRTF data is loaded
     * @return True if data is loaded, false otherwise
     */
    bool isLoaded() const { return loaded; }
    
    /**
     * @brief Checks if distance information is available
     * @return True if distance info is available, false otherwise
     */
    bool hasDistanceInfo() const { return distanceAvailable; }
    
    /**
     * @brief Gets the list of available distances in the dataset
     * @return Vector of available distances in meters
     */
    std::vector<float> getAvailableDistances() const { return availableDistances; }
    
    /**
     * @brief Gets the minimum distance in the dataset
     * @return Minimum distance in meters
     */
    float getMinDistance() const;
    
    /**
     * @brief Gets the maximum distance in the dataset
     * @return Maximum distance in meters
     */
    float getMaxDistance() const;
    
    /**
     * @brief Gets the nearest position index in the database
     * @param azimuth Azimuth angle in degrees
     * @param elevation Elevation angle in degrees
     * @return Index of nearest position
     */
    int getNearestPositionIndex(float azimuth, float elevation) const;
    
    /**
     * @brief Gets the nearest position index with distance consideration
     * @param azimuth Azimuth angle in degrees
     * @param elevation Elevation angle in degrees
     * @param distance Distance in meters
     * @return Index of nearest position
     */
    int getNearestPositionIndexWithDistance(float azimuth, float elevation, float distance) const;
    
    /**
     * @brief Creates a default HRTF dataset
     * 
     * Generates a minimal HRTF with basic spatial cues (ITD and ILD)
     * for common positions around the listener
     */
    void createDefaultHRTF();
    
private:
    /**
     * @brief Structure representing a measurement position
     */
    struct Position
    {
        float azimuth;   ///< Azimuth in degrees
        float elevation; ///< Elevation in degrees
        float distance;  ///< Distance in meters (typically 1.0)
    };
    
    /**
     * @brief Flag indicating if HRTF data is loaded
     */
    bool loaded;
    
    /**
     * @brief Sample rate of the HRTF dataset in Hz
     */
    int sampleRate;
    
    /**
     * @brief Length of impulse responses in samples
     */
    int irLength;
    
    /**
     * @brief Number of measurements in the dataset
     */
    int numMeasurements;
    
    /**
     * @brief Vector of measurement positions
     */
    std::vector<Position> positions;
    
    /**
     * @brief Vector of left ear impulse responses
     */
    std::vector<std::vector<float>> leftIRs;
    
    /**
     * @brief Vector of right ear impulse responses
     */
    std::vector<std::vector<float>> rightIRs;
    
    /**
     * @brief Flag indicating if distance information is available
     */
    bool distanceAvailable;
    
    /**
     * @brief Vector of unique available distances in the dataset
     */
    std::vector<float> availableDistances;
    
    /**
     * @brief Calculates angular distance between two positions
     * @param az1 Azimuth of first position in degrees
     * @param el1 Elevation of first position in degrees
     * @param az2 Azimuth of second position in degrees
     * @param el2 Elevation of second position in degrees
     * @return Angular distance in radians
     */
    float angularDistance(float az1, float el1, float az2, float el2) const;

    // Fix D: azimuth LUT — O(1) lookup replacing O(M trig) linear search
    static constexpr int LUT_SIZE = 360;
    int  m_azimuthLUT[LUT_SIZE] = {};
    bool m_lutBuilt = false;
    void buildAzimuthLUT();
};
