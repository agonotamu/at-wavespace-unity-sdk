/**
 * @file Main.cpp
 * @brief Interactive console application for testing binaural WFS audio library
 * @author Antoine Gonot
 * @date 2025-02-06
 */

#include <iostream>
#include <string>
#include <vector>
#include <cmath>
#include <limits>
#include <thread>
#include <chrono>

#include "AT_AudioManager.h"

// Platform-specific console clear
#ifdef _WIN32
#define CLEAR_CONSOLE "cls"
#else
#define CLEAR_CONSOLE "clear"
#endif

// Constants
constexpr float PI = 3.14159265358979323846f;

// Global audio manager
std::unique_ptr<AT::AudioManager> g_audioManager;

// Configuration state
struct AppConfig {
    std::string audioFilePath;
    bool isBinauralMode = false;
    bool isSimpleBinauralSpat = false;  // Simple binaural mode (A/B comparison)
    std::string hrtfFilePath;        // ← AJOUTÉ: Chemin HRTF
    std::string selectedDeviceName;
    int numOutputChannels = 64;
    float speakerCircleRadius = 2.0f;
    int playerUID = -1;
    
    // Listener state
    float listenerPosX = 0.0f;
    float listenerPosY = 0.0f;
    float listenerPosZ = 0.0f;
    float listenerRotationY = 0.0f;  // Rotation in degrees
    
    // Source state
    float sourcePosX = 0.0f;
    float sourcePosY = 0.0f;
    float sourcePosZ = 0.0f;
};

AppConfig g_config;

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================

void clearConsole() {
    system(CLEAR_CONSOLE);
}

void waitForEnter() {
    std::cout << "\nPress ENTER to continue...";
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    std::cin.get();
}

float degToRad(float degrees) {
    return degrees * PI / 180.0f;
}

void printSeparator() {
    std::cout << "\n========================================\n";
}

// ============================================================================
// SETUP FUNCTIONS
// ============================================================================

std::string promptAudioFilePath() {
    std::string path;
    std::cout << "\n=== AUDIO FILE SELECTION ===\n";
    std::cout << "Enter the full path to the audio file:\n";
    std::cout << "> ";
    std::getline(std::cin, path);
    return path;
}

bool promptBinauralMode() {
    char choice;
    std::cout << "\n=== BINAURAL VIRTUALIZATION ===\n";
    std::cout << "Enable binaural virtualization (WFS N-channels -> Stereo 2-channels)?\n";
    std::cout << "[y] Yes - Binaural mode (2-channel output)\n";
    std::cout << "[n] No  - Direct mode (N-channel output)\n";
    std::cout << "> ";
    std::cin >> choice;
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    
    return (choice == 'y' || choice == 'Y');
}

bool promptSimpleBinauralMode() {
    char choice;
    std::cout << "\n=== SIMPLE BINAURAL MODE (A/B TEST) ===\n";
    std::cout << "Enable simple binaural spatialization?\n";
    std::cout << "[y] Yes - Simple binaural (direct HRTF on source)\n";
    std::cout << "[n] No  - WFS + Binaural virtualization\n";
    std::cout << "\nNote: This allows A/B comparison between WFS and simple panning.\n";
    std::cout << "> ";
    std::cin >> choice;
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    
    return (choice == 'y' || choice == 'Y');
}

// ════════════════════════════════════════════════════════════════════════════
// NOUVELLE FONCTION: Prompt HRTF file path
// ════════════════════════════════════════════════════════════════════════════
std::string promptHRTFFilePath() {
    std::string path;
    std::cout << "\n=== HRTF FILE SELECTION ===\n";
    std::cout << "Enter the full path to the HRTF data file (.txt format):\n";
    std::cout << "(Leave empty to use built-in default HRTF)\n";
    std::cout << "> ";
    std::getline(std::cin, path);
    return path;
}

std::string promptAudioDevice() {
    std::cout << "\n=== AUDIO DEVICE SELECTION ===\n";
    
    // Platform-specific device scanning
    // CRITICAL: Use async=false for console apps (no JUCE message loop)
    /* // BUG ON WINDOWS
    #if JUCE_WINDOWS
        std::cout << "Scanning available audio devices (including ASIO)...\n";
        // Windows: Scan ASIO devices (important for pro audio interfaces)
        g_audioManager->refreshDevices(true, false);  // includeASIO=true, async=false
    #else
        std::cout << "Scanning available audio devices...\n";
        // macOS/Linux: No ASIO available
        g_audioManager->refreshDevices(false, false);  // includeASIO=false, async=false
    #endif

    std::cout << "Device scan complete.\n";
    */

#if JUCE_WINDOWS
    // ════════════════════════════════════════════════════════════════════
    // WINDOWS: Scan with ASIO + query channel counts
    // ════════════════════════════════════════════════════════════════════
    std::cout << "Scanning available audio devices (including ASIO)...\n";
    g_audioManager->refreshDevices(true, false);  // includeASIO=true
    std::cout << "Device scan complete.\n";

    // FIX WINDOWS: Query actual channel counts
    // refreshDevices() uses lazy loading (-1), so we must query each device
    int deviceCount = g_audioManager->getCachedDeviceCount();
    std::cout << "Querying device capabilities (" << deviceCount << " devices)...\n";
    for (int i = 0; i < deviceCount; i++) {
        g_audioManager->getDetailedDeviceInfo(i);
    }
    std::cout << "Device query complete.\n";
#else
    // ════════════════════════════════════════════════════════════════════
    // macOS/Linux: Scan without ASIO
    // ════════════════════════════════════════════════════════════════════
    std::cout << "Scanning available audio devices...\n";
    g_audioManager->refreshDevices(false, false);  // includeASIO=false
    std::cout << "Device scan complete.\n";
    
    // Get device count
    int deviceCount = g_audioManager->getCachedDeviceCount();
    
    // FIX macOS: Query actual channel counts (same as Windows)
    // refreshDevices() uses lazy loading (-1), so we must query each device
    std::cout << "Querying device capabilities (" << deviceCount << " devices)...\n";
    for (int i = 0; i < deviceCount; i++) {
        g_audioManager->getDetailedDeviceInfo(i);
    }
    std::cout << "Device query complete.\n";
#endif

    // ════════════════════════════════════════════════════════════════════
    // COMMON: Check if devices were found
    // ════════════════════════════════════════════════════════════════════
    if (deviceCount == 0) {
        std::cout << "\nNo audio devices found!\n";
        return "";
    }
    
    // Display devices
    std::cout << "\nAvailable audio devices:\n";
    for (int i = 0; i < deviceCount; i++) {
        AT::DeviceInfo device = g_audioManager->getCachedDeviceInfo(i);
       if (device.maxOutputChannels > 0) {
            std::cout << "[" << i << "] " << device.name << " (" << device.typeName << ")\n";


#if JUCE_WINDOWS
            // On Windows, highlight ASIO devices
            if (device.typeName == "ASIO") {
                std::cout << "    *** ASIO (Low Latency) ***\n";
            }
#endif

            std::cout << "    Inputs: " << device.maxInputChannels
                << ", Outputs: " << device.maxOutputChannels << "\n";

        }
    }
    
    // Prompt selection
    int selection;
    std::cout << "\nEnter device index (or -1 for default): ";
    std::cin >> selection;
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    
    if (selection < 0 || selection >= deviceCount) {
        std::cout << "Using system default device.\n";
        return "";
    }
    
    AT::DeviceInfo selectedDevice = g_audioManager->getCachedDeviceInfo(selection);
    std::cout << "Selected: " << selectedDevice.name << "\n";
    return selectedDevice.name;
}

int promptNumChannels() {
    int numChannels;
    std::cout << "\n=== WFS CHANNEL COUNT ===\n";
    std::cout << "Enter number of virtual speakers for WFS (e.g., 32, 64, 128):\n";
    std::cout << "> ";
    std::cin >> numChannels;
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    
    if (numChannels < 2) {
        std::cout << "Warning: Minimum 2 channels required. Setting to 2.\n";
        numChannels = 2;
    }
    
    return numChannels;
}

float promptSpeakerRadius() {
    float radius;
    std::cout << "\n=== SPEAKER CIRCLE DIAMETER ===\n";
    std::cout << "Enter diameter of speaker circle in meters (e.g., 4.0):\n";
    std::cout << "> ";
    std::cin >> radius;
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    
    // Convert diameter to radius
    radius /= 2.0f;
    
    if (radius < 0.5f) {
        std::cout << "Warning: Radius too small. Setting to 0.5m.\n";
        radius = 0.5f;
    }
    
    std::cout << "Speaker circle radius: " << radius << "m\n";
    return radius;
}

void setupVirtualSpeakers() {
    std::cout << "\n=== SETTING UP VIRTUAL SPEAKERS ===\n";
    std::cout << "Creating " << g_config.numOutputChannels << " speakers in a circle...\n";
    std::cout << "Radius: " << g_config.speakerCircleRadius << "m\n";
    
    // Allocate arrays for positions, rotations, forwards
    int numSpeakers = g_config.numOutputChannels;
    std::vector<float> positions(numSpeakers * 3);
    std::vector<float> rotations(numSpeakers * 3);
    std::vector<float> forwards(numSpeakers * 3);
    
    // Calculate positions in a circle around (0,0,0)
    for (int i = 0; i < numSpeakers; i++) {
        float angle = (i * 2.0f * PI) / numSpeakers;
        
        // Position (circle in XZ plane)
        positions[i * 3 + 0] = g_config.speakerCircleRadius * std::sin(angle);  // X
        positions[i * 3 + 1] = 0.0f;                                             // Y
        positions[i * 3 + 2] = g_config.speakerCircleRadius * std::cos(angle);  // Z
        
        // Rotation (all zeros for now)
        rotations[i * 3 + 0] = 0.0f;
        rotations[i * 3 + 1] = angle * 180.0f / PI;  // Y rotation in degrees
        rotations[i * 3 + 2] = 0.0f;
        
        // Forward vector (pointing toward center)
        forwards[i * 3 + 0] = -std::sin(angle);
        forwards[i * 3 + 1] = 0.0f;
        forwards[i * 3 + 2] = -std::cos(angle);
    }
    
    // Set virtual speakers (also sets virtual mics - same positions)
    g_audioManager->setVirtualSpeakerTransform(
        positions.data(),
        rotations.data(),
        forwards.data(),
        numSpeakers
    );
        
    std::cout << "Virtual speakers configured successfully.\n";
    std::cout << "Speaker 0 position: (" 
              << positions[0] << ", " 
              << positions[1] << ", " 
              << positions[2] << ")\n";
}

void setupListener() {
    std::cout << "\n=== SETTING UP LISTENER ===\n";
    
    // Initial position (0,0,0)
    g_config.listenerPosX = -1.0f;
    g_config.listenerPosY = 0.0f;
    g_config.listenerPosZ = 0.0f;
    g_config.listenerRotationY = 0.0f;
    
    float position[3] = {0.0f, 0.0f, 0.0f};
    float rotation[3] = {0.0f, 0.0f, 0.0f};
    float forward[3] = {0.0f, 0.0f, 1.0f};  // Looking toward +Z
    
    g_audioManager->setListenerTransform(position, rotation, forward);
    
    std::cout << "Listener position: (0, 0, 0)\n";
    std::cout << "Listener forward: (0, 0, 1) - Looking toward +Z\n";
}

void setupSource() {
    std::cout << "\n=== SETTING UP AUDIO SOURCE ===\n";
    
    // Position source in front of listener (at radius + 2 meters)
    g_config.sourcePosX = 0.0f;
    g_config.sourcePosY = 0.0f;
    g_config.sourcePosZ = 0.0f;//g_config.speakerCircleRadius + 2.0f;
    
    float position[3] = {
        g_config.sourcePosX,
        g_config.sourcePosY,
        g_config.sourcePosZ
    };
    float rotation[3] = {0.0f, 0.0f, 0.0f};
    float forward[3] = {0.0f, 0.0f, 1.0f};
    
    g_audioManager->setPlayerTransform(g_config.playerUID, position, rotation, forward);
    
    std::cout << "Source position: (" 
              << g_config.sourcePosX << ", " 
              << g_config.sourcePosY << ", " 
              << g_config.sourcePosZ << ")\n";
}

bool initializeAudioSystem() {
    printSeparator();
    std::cout << "=== AUDIO SYSTEM INITIALIZATION ===\n";
    printSeparator();
    
    // 1. Audio file path
    g_config.audioFilePath = promptAudioFilePath();
    if (g_config.audioFilePath.empty()) {
        std::cout << "Error: No file path provided.\n";
        return false;
    }
    
    // 2. Binaural mode
    g_config.isBinauralMode = promptBinauralMode();
    std::cout << "Mode: " << (g_config.isBinauralMode ? "BINAURAL (2ch output)" : "DIRECT (Nch output)") << "\n";
    
    // ════════════════════════════════════════════════════════════════════
    // 3. HRTF file (only if binaural mode)
    // ════════════════════════════════════════════════════════════════════
    if (g_config.isBinauralMode) {
        g_config.hrtfFilePath = promptHRTFFilePath();
        if (g_config.hrtfFilePath.empty()) {
            std::cout << "No HRTF file specified - will use default built-in HRTF.\n";
        } else {
            std::cout << "HRTF file: " << g_config.hrtfFilePath << "\n";
        }
        
        // 3b. Simple Binaural mode (A/B test)
        g_config.isSimpleBinauralSpat = promptSimpleBinauralMode();
        std::cout << "Simple Binaural: " << (g_config.isSimpleBinauralSpat ? "ENABLED" : "DISABLED") << "\n";
    }
    
    // 4. Number of channels
    g_config.numOutputChannels = promptNumChannels();
    
    // 5. Speaker radius
    g_config.speakerCircleRadius = promptSpeakerRadius();
    
    // 6. Audio device
    g_config.selectedDeviceName = promptAudioDevice();
    
    printSeparator();
    std::cout << "\n=== CONFIGURATION SUMMARY ===\n";
    std::cout << "Audio file: " << g_config.audioFilePath << "\n";
    std::cout << "Mode: " << (g_config.isBinauralMode ? "Binaural (2ch)" : "Direct (Nch)") << "\n";
    if (g_config.isBinauralMode) {
        std::cout << "HRTF: " << (g_config.hrtfFilePath.empty() ? "Default (built-in)" : g_config.hrtfFilePath) << "\n";
    }
    std::cout << "Virtual speakers: " << g_config.numOutputChannels << "\n";
    std::cout << "Speaker radius: " << g_config.speakerCircleRadius << "m\n";
    std::cout << "Device: " << (g_config.selectedDeviceName.empty() ? "DEFAULT" : g_config.selectedDeviceName) << "\n";
    printSeparator();
    
    waitForEnter();
    
    // Initialize audio manager
    std::cout << "\nInitializing audio engine...\n";
    
   
    
    // Setup audio engine (this creates HRTF processors)
    g_audioManager->setup(
        g_config.selectedDeviceName,
        0,  // No input channels
        g_config.numOutputChannels,  // Virtual speakers
        512,  // Buffer size
        g_config.isBinauralMode // binaural virtualization
    );


    std::cout << "Audio engine initialized.\n";
    
    // ════════════════════════════════════════════════════════════════════
    // CRITICAL: Load HRTF AFTER setup (processors must exist)
    // ════════════════════════════════════════════════════════════════════
    if (g_config.isBinauralMode) {
        std::cout << "\n=== LOADING HRTF ===\n";
        
        bool hrtfLoaded = false;
        
        if (!g_config.hrtfFilePath.empty()) {
            std::cout << "Loading custom HRTF from: " << g_config.hrtfFilePath << "\n";
            
            // Get spatialization engine and load HRTF
            auto* engine = g_audioManager->getSpatializationEngine();
            hrtfLoaded = engine->loadHRTFFile(g_config.hrtfFilePath);
            
            if (hrtfLoaded) {
                std::cout << "✓ Custom HRTF loaded successfully!\n";
            } else {
                std::cout << "✗ Failed to load custom HRTF file.\n";
                std::cout << "  Falling back to default HRTF...\n";
            }
        }
        
        // Load default HRTF if custom failed or not specified
        if (!hrtfLoaded) {
            std::cout << "Loading default built-in HRTF...\n";
            auto* engine = g_audioManager->getSpatializationEngine();
            hrtfLoaded = engine->loadDefaultHRTF();
            
            if (hrtfLoaded) {
                std::cout << "✓ Default HRTF loaded successfully!\n";
            } else {
                std::cout << "✗ WARNING: Failed to load default HRTF!\n";
                std::cout << "  Audio may have glitches or incorrect spatialization.\n";
            }
        }
        
        printSeparator();
        
        // ════════════════════════════════════════════════════════════════════
        // Set simple binaural mode if requested
        // ════════════════════════════════════════════════════════════════════
        if (g_config.isSimpleBinauralSpat) {
            std::cout << "\n=== ENABLING SIMPLE BINAURAL MODE ===\n";
            std::cout << "Bypassing WFS - applying HRTF directly to source...\n";
            
            auto* engine = g_audioManager->getSpatializationEngine();
            bool success = engine->setIsSimpleBinauralSpat(true);
            
            if (success) {
                std::cout << "✓ Simple binaural mode ENABLED\n";
                std::cout << "  Mode: Direct HRTF convolution (no WFS)\n";
            } else {
                std::cout << "✗ WARNING: Failed to enable simple binaural mode!\n";
                std::cout << "  Falling back to WFS + binaural virtualization\n";
                g_config.isSimpleBinauralSpat = false;
            }
            
            printSeparator();
        }
    }
    
    // Setup virtual speakers
    setupVirtualSpeakers();
    
    // Setup listener
    setupListener();
    
    // Create player
    std::cout << "\nCreating audio player...\n";
    g_audioManager->addPlayer(&g_config.playerUID, true, true);  // 3D, looping
    
    if (g_config.playerUID < 0) {
        std::cout << "Error: Failed to create player.\n";
        return false;
    }
    
    std::cout << "Player created (UID: " << g_config.playerUID << ")\n";
    
    // Load audio file
    std::cout << "Loading audio file...\n";
    if (!g_audioManager->setPlayerFilePath(g_config.playerUID, g_config.audioFilePath.c_str())) {
        std::cout << "Error: Failed to load audio file.\n";
        return false;
    }
    
    std::cout << "Audio file loaded successfully.\n";
    
    // Setup source position
    setupSource();
    
    // set spatialization options :
    g_audioManager->setIsPrefilter(g_config.playerUID, true);
    g_audioManager->setIsWfsGain(true);
    g_audioManager->setIsNearFieldCorrection(true);
    
    // Start playback
    std::cout << "\nStarting playback...\n";
    g_audioManager->startPlayer(g_config.playerUID);
    
    std::cout << "\n✓ Audio system ready!\n";
    
    return true;
}

// ============================================================================
// INTERACTIVE CONTROLS
// ============================================================================

void updateListenerTransform() {
    float position[3] = {
        g_config.listenerPosX,
        g_config.listenerPosY,
        g_config.listenerPosZ
    };
    
    // Convert rotation to forward vector
    float angleRad = degToRad(g_config.listenerRotationY);
    float forward[3] = {
        std::sin(angleRad),
        0.0f,
        std::cos(angleRad)
    };
    
    float rotation[3] = {
        0.0f,
        g_config.listenerRotationY,
        0.0f
    };
    
    g_audioManager->setListenerTransform(position, rotation, forward);
}

void updateSourcePosition() {
    float position[3] = {
        g_config.sourcePosX,
        g_config.sourcePosY,
        g_config.sourcePosZ
    };
    float rotation[3] = {0.0f, 0.0f, 0.0f};
    float forward[3] = {0.0f, 0.0f, 1.0f};
    
    g_audioManager->setPlayerTransform(g_config.playerUID, position, rotation, forward);
}

void handleSourcePositionChange() {
    std::cout << "\n=== CHANGE SOURCE POSITION ===\n";
    std::cout << "Current position: ("
              << g_config.sourcePosX << ", "
              << g_config.sourcePosY << ", "
              << g_config.sourcePosZ << ")\n";
    
    std::cout << "Enter new X coordinate: ";
    std::cin >> g_config.sourcePosX;
    std::cout << "Enter new Y coordinate: ";
    std::cin >> g_config.sourcePosY;
    std::cout << "Enter new Z coordinate: ";
    std::cin >> g_config.sourcePosZ;
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    
    updateSourcePosition();
    
    std::cout << "✓ Source moved to: ("
              << g_config.sourcePosX << ", "
              << g_config.sourcePosY << ", "
              << g_config.sourcePosZ << ")\n";
}

void handleListenerRotation() {
    std::cout << "\n=== ROTATE LISTENER ===\n";
    std::cout << "Current rotation: " << g_config.listenerRotationY << "°\n";
    std::cout << "Enter new rotation angle (degrees, 0-360): ";
    std::cin >> g_config.listenerRotationY;
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    
    // Normalize angle to 0-360
    while (g_config.listenerRotationY < 0) g_config.listenerRotationY += 360.0f;
    while (g_config.listenerRotationY >= 360) g_config.listenerRotationY -= 360.0f;
    
    updateListenerTransform();
    
    float angleRad = degToRad(g_config.listenerRotationY);
    std::cout << "✓ Listener rotated to: " << g_config.listenerRotationY << "°\n";
    std::cout << "  New forward: ("
              << std::sin(angleRad) << ", 0, "
              << std::cos(angleRad) << ")\n";
}

void handleListenerPositionChange() {
    std::cout << "\n=== CHANGE LISTENER POSITION ===\n";
    std::cout << "Current position: ("
              << g_config.listenerPosX << ", "
              << g_config.listenerPosY << ", "
              << g_config.listenerPosZ << ")\n";
    
    std::cout << "Enter new X coordinate: ";
    std::cin >> g_config.listenerPosX;
    std::cout << "Enter new Y coordinate: ";
    std::cin >> g_config.listenerPosY;
    std::cout << "Enter new Z coordinate: ";
    std::cin >> g_config.listenerPosZ;
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    
    updateListenerTransform();
    
    std::cout << "✓ Listener moved to: ("
              << g_config.listenerPosX << ", "
              << g_config.listenerPosY << ", "
              << g_config.listenerPosZ << ")\n";
}

void handleToggleSimpleBinaural() {
    if (!g_config.isBinauralMode) {
        std::cout << "\n✗ Simple binaural mode only available in binaural virtualization mode!\n";
        return;
    }
    
    // Toggle the flag
    g_config.isSimpleBinauralSpat = !g_config.isSimpleBinauralSpat;
    
    std::cout << "\n=== TOGGLE SIMPLE BINAURAL MODE ===\n";
    std::cout << "Switching to: " << (g_config.isSimpleBinauralSpat ? "Simple Binaural" : "WFS + Binaural") << "\n";
    
    // Apply change
    auto* engine = g_audioManager->getSpatializationEngine();
    bool success = engine->setIsSimpleBinauralSpat(g_config.isSimpleBinauralSpat);
    
    if (success) {
        if (g_config.isSimpleBinauralSpat) {
            std::cout << "✓ Simple Binaural mode ENABLED\n";
            std::cout << "  → Direct HRTF convolution (bypassing WFS)\n";
        } else {
            std::cout << "✓ WFS + Binaural mode ENABLED\n";
            std::cout << "  → WFS spatialization + HRTF virtualization\n";
        }
    } else {
        std::cout << "✗ Failed to change mode!\n";
        g_config.isSimpleBinauralSpat = !g_config.isSimpleBinauralSpat; // Revert
    }
}

void displayStatus() {
    clearConsole();
    
    printSeparator();
    std::cout << "=== CURRENT STATUS ===\n";
    printSeparator();
    
    std::cout << "\nAUDIO CONFIGURATION:\n";
    std::cout << "  Mode: " << (g_config.isBinauralMode ? "Binaural (2ch)" : "Direct (Nch)") << "\n";
    if (g_config.isBinauralMode) {
        std::cout << "  Spatialization: " << (g_config.isSimpleBinauralSpat ? "Simple Binaural (Direct HRTF)" : "WFS + Binaural") << "\n";
    }
    std::cout << "  Virtual speakers: " << g_config.numOutputChannels << "\n";
    std::cout << "  Speaker radius: " << g_config.speakerCircleRadius << "m\n";
    
    std::cout << "\nLISTENER:\n";
    std::cout << "  Position: ("
              << g_config.listenerPosX << ", "
              << g_config.listenerPosY << ", "
              << g_config.listenerPosZ << ")\n";
    std::cout << "  Rotation: " << g_config.listenerRotationY << "°\n";
    float angleRad = degToRad(g_config.listenerRotationY);
    std::cout << "  Forward: ("
              << std::sin(angleRad) << ", 0, "
              << std::cos(angleRad) << ")\n";
    
    std::cout << "\nSOURCE:\n";
    std::cout << "  Position: ("
              << g_config.sourcePosX << ", "
              << g_config.sourcePosY << ", "
              << g_config.sourcePosZ << ")\n";
    
    // Calculate distance
    float dx = g_config.sourcePosX - g_config.listenerPosX;
    float dy = g_config.sourcePosY - g_config.listenerPosY;
    float dz = g_config.sourcePosZ - g_config.listenerPosZ;
    float distance = std::sqrt(dx*dx + dy*dy + dz*dz);
    std::cout << "  Distance from listener: " << distance << "m\n";
    
    printSeparator();
}

void showMenu() {
    std::cout << "\n=== CONTROLS ===\n";
    std::cout << "[s] Change source position (X, Y, Z)\n";
    std::cout << "[r] Rotate listener (angle in degrees)\n";
    std::cout << "[l] Change listener position (X, Y, Z)\n";
    if (g_config.isBinauralMode) {
        std::cout << "[b] Toggle Simple Binaural / WFS mode (A/B comparison)\n";
    }
    std::cout << "[i] Show current status\n";
    std::cout << "[q] Quit program\n";
    std::cout << "\nEnter command: ";
}

void interactiveLoop() {
    char command;
    bool running = true;
    
    std::cout << "\n";
    printSeparator();
    std::cout << "=== INTERACTIVE MODE ===\n";
    std::cout << "Audio is now playing in loop.\n";
    std::cout << "Use commands to control the scene.\n";
    printSeparator();
    
    displayStatus();
    
    while (running) {
        showMenu();
        std::cin >> command;
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        
        switch (command) {
            case 's':
            case 'S':
                handleSourcePositionChange();
                break;
                
            case 'r':
            case 'R':
                handleListenerRotation();
                break;
                
            case 'l':
            case 'L':
                handleListenerPositionChange();
                break;
                
            case 'b':
            case 'B':
                handleToggleSimpleBinaural();
                break;
                
            case 'i':
            case 'I':
                displayStatus();
                break;
                
            case 'q':
            case 'Q':
                std::cout << "\nExiting...\n";
                running = false;
                break;
                
            default:
                std::cout << "\nUnknown command. Try again.\n";
                break;
        }
    }
}

// ============================================================================
// MAIN
// ============================================================================

int main(int argc, char* argv[]) {
    std::cout << "========================================\n";
    std::cout << "  WFS BINAURAL AUDIO TEST APPLICATION\n";
    std::cout << "========================================\n";
    std::cout << "\nThis application tests the binaural WFS audio library.\n";
    std::cout << "You can control listener and source positions in real-time.\n";
    
    waitForEnter();
    
    // Create audio manager
    g_audioManager = std::make_unique<AT::AudioManager>();
    
    // Initialize system with user configuration
    if (!initializeAudioSystem()) {
        std::cout << "\nFailed to initialize audio system.\n";
        waitForEnter();
        return 1;
    }
    
    // Enter interactive loop
    interactiveLoop();
    
    // Cleanup
    std::cout << "\nStopping playback...\n";
    g_audioManager->stopPlayer(g_config.playerUID);
    
    std::cout << "Removing player...\n";
    g_audioManager->removePlayer(g_config.playerUID);
    
    std::cout << "Stopping audio engine...\n";
    g_audioManager->stop();
    
    // CRITICAL: Explicitly destroy AudioManager BEFORE main() exits
    // This prevents ShutdownDetector leak caused by destruction after JUCE statics
    std::cout << "Cleaning up audio manager...\n";
    g_audioManager.reset();  // Destroy AudioManager while JUCE is still valid
    
    std::cout << "\nGoodbye!\n";
    
    return 0;
}
