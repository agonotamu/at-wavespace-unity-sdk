#include "SOFAReader.h"
#include <cmath>
#include <fstream>
#include <sstream>
#include <set>
#include <algorithm>
#include "AT_SpatConfig.h"

SOFAReader::SOFAReader()
    : loaded(false)
    , sampleRate(44100)
    , irLength(256)
    , numMeasurements(0)
    , distanceAvailable(false)
{
}

SOFAReader::~SOFAReader()
{
}

bool SOFAReader::loadFile(const juce::File& file)
{
    if (!file.existsAsFile())
        return false;
    
    // Simple text format
    // HEADER <sampleRate> <irLength>
    // <azimuth> <elevation> [distance] <leftIR...> <rightIR...>
    
    std::ifstream input(file.getFullPathName().toStdString());
    if (!input.is_open())
        return false;
    
    positions.clear();
    leftIRs.clear();
    rightIRs.clear();
    availableDistances.clear();
    distanceAvailable = false;
    
    std::string line;
    
    // Read header
    if (std::getline(input, line))
    {
        std::istringstream iss(line);
        std::string header;
        iss >> header >> sampleRate >> irLength;
    }
    
    // Read measurements
    while (std::getline(input, line))
    {
        if (line.empty() || line[0] == '#')
            continue;
            
        std::istringstream iss(line);
        
        Position pos;
        iss >> pos.azimuth >> pos.elevation;
        
        // Try to read distance (optional)
        float potentialDistance;
        std::streampos oldPos = iss.tellg();
        
        if (iss >> potentialDistance)
        {
            // Check if it's a valid distance (> 0 and < 100m)
            if (potentialDistance > 0.0f && potentialDistance < 100.0f)
            {
                pos.distance = potentialDistance;
                distanceAvailable = true;
            }
            else
            {
                // Not a distance, go back
                iss.seekg(oldPos);
                pos.distance = 1.0f;
            }
        }
        else
        {
            pos.distance = 1.0f;
        }
        
        std::vector<float> leftIR(irLength);
        std::vector<float> rightIR(irLength);
        
        for (int i = 0; i < irLength; ++i)
            iss >> leftIR[i];
            
        for (int i = 0; i < irLength; ++i)
            iss >> rightIR[i];
        
        positions.push_back(pos);
        leftIRs.push_back(leftIR);
        rightIRs.push_back(rightIR);
    }
    
    numMeasurements = static_cast<int>(positions.size());
    loaded = numMeasurements > 0;
    
    // Extract unique distances if available
    if (distanceAvailable && loaded)
    {
        std::set<float> uniqueDistances;
        for (const auto& pos : positions)
        {
            uniqueDistances.insert(pos.distance);
        }
        availableDistances.assign(uniqueDistances.begin(), uniqueDistances.end());
        std::sort(availableDistances.begin(), availableDistances.end());
    }
    
    // If no data was loaded, create a minimal default HRTF
    if (!loaded)
    {
        createDefaultHRTF();
    }

    if (loaded)
        buildAzimuthLUT();

    return loaded;
}

void SOFAReader::createDefaultHRTF()
{
    // Create a minimal HRTF with a few positions
    sampleRate = 44100;
    irLength = 128;
    
    positions.clear();
    leftIRs.clear();
    rightIRs.clear();
    
    // Basic positions (azimuth, elevation)
    std::vector<std::pair<float, float>> defaultPositions = {
        {0.0f, 0.0f},      // Center
        {-90.0f, 0.0f},    // Left
        {90.0f, 0.0f},     // Right
        {-45.0f, 0.0f},    // Front-left
        {45.0f, 0.0f},     // Front-right
        {-135.0f, 0.0f},   // Rear-left
        {135.0f, 0.0f},    // Rear-right
        {180.0f, 0.0f},    // Rear
        {-30.0f, 0.0f},    // Intermediate positions
        {30.0f, 0.0f},
        {-60.0f, 0.0f},
        {60.0f, 0.0f},
        {-120.0f, 0.0f},
        {120.0f, 0.0f},
        {-150.0f, 0.0f},
        {150.0f, 0.0f}
    };
    
    for (const auto& posData : defaultPositions)
    {
        Position pos;
        pos.azimuth = posData.first;
        pos.elevation = posData.second;
        pos.distance = 1.0f;
        
        positions.push_back(pos);
        
        // Create simplified IRs with ITD and ILD
        float azRad = pos.azimuth * juce::MathConstants<float>::pi / 180.0f;
        
        // Calculate interaural time delay (ITD) in samples
        float headRadius = 0.0875f; // Average head radius in meters
        float speedOfSound = 343.0f; // m/s
        float itdSeconds = (headRadius / speedOfSound) * (azRad + std::sin(azRad));
        int itdSamples = static_cast<int>(std::abs(itdSeconds * sampleRate));
        itdSamples = juce::jlimit(0, irLength / 3, itdSamples);
        
        // Calculate interaural level difference (ILD) in dB - more pronounced
        float ild = 15.0f * std::abs(std::sin(azRad));
        float leftGain = azRad > 0 ? juce::Decibels::decibelsToGain(-ild) : 1.0f;
        float rightGain = azRad < 0 ? juce::Decibels::decibelsToGain(-ild) : 1.0f;
        
        std::vector<float> leftIR(irLength, 0.0f);
        std::vector<float> rightIR(irLength, 0.0f);
        
        // Create a simple impulse response (impulse with envelope)
        int leftDelay = azRad > 0 ? itdSamples : 0;
        int rightDelay = azRad < 0 ? itdSamples : 0;
        
        // Left ear
        for (int i = 0; i < irLength; ++i)
        {
            if (i >= leftDelay && i < leftDelay + 5)
            {
                float env = std::exp(-(i - leftDelay) / 10.0f);
                leftIR[i] = leftGain * env * 0.8f;
            }
        }
        
        // Right ear
        for (int i = 0; i < irLength; ++i)
        {
            if (i >= rightDelay && i < rightDelay + 5)
            {
                float env = std::exp(-(i - rightDelay) / 10.0f);
                rightIR[i] = rightGain * env * 0.8f;
            }
        }
        
        leftIRs.push_back(leftIR);
        rightIRs.push_back(rightIR);
    }
    
    numMeasurements = static_cast<int>(positions.size());
    loaded = true;
    buildAzimuthLUT();
}

bool SOFAReader::getIRsForPosition(float azimuth, float elevation,
                                   std::vector<float>& leftIR,
                                   std::vector<float>& rightIR)
{
    if (!loaded || numMeasurements == 0)
        return false;
    
    int idx = getNearestPositionIndex(azimuth, elevation);
    
    if (idx >= 0 && idx < numMeasurements)
    {
        leftIR = leftIRs[idx];
        rightIR = rightIRs[idx];
        return true;
    }
    
    return false;
}

void SOFAReader::buildAzimuthLUT()
{
    m_lutBuilt = false;
    if (numMeasurements == 0) return;

    for (int deg = 0; deg < LUT_SIZE; ++deg)
    {
        float az = static_cast<float>(deg);
        int bestIdx = 0;
        float bestDist = std::numeric_limits<float>::max();
        for (int i = 0; i < numMeasurements; ++i)
        {
            float d = angularDistance(az, 0.0f, positions[i].azimuth, positions[i].elevation);
            if (d < bestDist) { bestDist = d; bestIdx = i; }
        }
        m_azimuthLUT[deg] = bestIdx;
    }
    m_lutBuilt = true;
}

int SOFAReader::getNearestPositionIndex(float azimuth, float elevation) const
{
    if (numMeasurements == 0)
        return -1;

    // Fix D: O(1) LUT fast path for elevation ≈ 0 (all WFS horizontal speakers).
    if (m_lutBuilt && std::abs(elevation) < 1.0f)
    {
        float norm = azimuth;
        while (norm <    0.0f) norm += 360.0f;
        while (norm >= 360.0f) norm -= 360.0f;
        return m_azimuthLUT[static_cast<int>(norm + 0.5f) % LUT_SIZE];
    }

    // Slow path: full O(M) search for non-zero elevation or missing LUT.
    float norm = azimuth;
    while (norm <    0.0f) norm += 360.0f;
    while (norm >= 360.0f) norm -= 360.0f;

    int nearestIdx = 0;
    float minDist = std::numeric_limits<float>::max();
    for (int i = 0; i < numMeasurements; ++i)
    {
        float dist = angularDistance(norm, elevation, positions[i].azimuth, positions[i].elevation);
        if (dist < minDist) { minDist = dist; nearestIdx = i; }
    }
    return nearestIdx;
}

float SOFAReader::angularDistance(float az1, float el1, float az2, float el2) const
{
    // Convert to radians
    float az1Rad = az1 * juce::MathConstants<float>::pi / 180.0f;
    float el1Rad = el1 * juce::MathConstants<float>::pi / 180.0f;
    float az2Rad = az2 * juce::MathConstants<float>::pi / 180.0f;
    float el2Rad = el2 * juce::MathConstants<float>::pi / 180.0f;
    
    // Convert to Cartesian coordinates on unit sphere
    float x1 = std::cos(el1Rad) * std::cos(az1Rad);
    float y1 = std::cos(el1Rad) * std::sin(az1Rad);
    float z1 = std::sin(el1Rad);
    
    float x2 = std::cos(el2Rad) * std::cos(az2Rad);
    float y2 = std::cos(el2Rad) * std::sin(az2Rad);
    float z2 = std::sin(el2Rad);
    
    // Dot product
    float dot = x1 * x2 + y1 * y2 + z1 * z2;
    dot = juce::jlimit(-1.0f, 1.0f, dot);
    
    // Angular distance
    return std::acos(dot);
}

bool SOFAReader::getIRsForPositionWithDistance(float azimuth, float elevation, float distance,
                                                std::vector<float>& leftIR,
                                                std::vector<float>& rightIR)
{
    if (!loaded || numMeasurements == 0)
        return false;
    
    int idx = getNearestPositionIndexWithDistance(azimuth, elevation, distance);
    
    if (idx >= 0 && idx < numMeasurements)
    {
        leftIR = leftIRs[idx];
        rightIR = rightIRs[idx];
        return true;
    }
    
    return false;
}

int SOFAReader::getNearestPositionIndexWithDistance(float azimuth, float elevation, float distance) const
{
    if (numMeasurements == 0)
        return -1;
    
    // If no distance info, ignore distance parameter
    if (!distanceAvailable)
        return getNearestPositionIndex(azimuth, elevation);
    
    // Normalize requested azimuth to 0-360 range
    float normalizedAzimuth = azimuth;
    while (normalizedAzimuth < 0.0f)
        normalizedAzimuth += 360.0f;
    while (normalizedAzimuth >= 360.0f)
        normalizedAzimuth -= 360.0f;
    
    int nearestIdx = 0;
    float minCombinedDistance = std::numeric_limits<float>::max();
    
    for (int i = 0; i < numMeasurements; ++i)
    {
        // Angular distance
        float angDist = angularDistance(normalizedAzimuth, elevation, 
                                        positions[i].azimuth, 
                                        positions[i].elevation);
        
        // Euclidean distance (normalized)
        float distDiff = std::abs(distance - positions[i].distance) / 10.0f; // Normalize by 10m
        
        // Combination: prioritize angular distance
        float combinedDist = angDist + distDiff * 0.1f;
        
        if (combinedDist < minCombinedDistance)
        {
            minCombinedDistance = combinedDist;
            nearestIdx = i;
        }
    }
    
    return nearestIdx;
}

float SOFAReader::getMinDistance() const
{
    if (!distanceAvailable || availableDistances.empty())
        return 1.0f;
    return availableDistances.front();
}

float SOFAReader::getMaxDistance() const
{
    if (!distanceAvailable || availableDistances.empty())
        return 1.0f;
    return availableDistances.back();
}
