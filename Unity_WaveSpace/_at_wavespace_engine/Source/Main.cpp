/**
 * @file Main6dof.cpp
 * @brief Minimal console application for testing 2D playback + 6DOF source
 *        masking (AT_SixDofMaskProcessor), amplitude-panning stereo downmix.
 * @author Antoine Gonot / Claude
 * @date 2026
 *
 * Deliberately stripped down from the original WFS/binaural test Main.cpp:
 * no 3D/WFS player, no HRTF prompts, no source-position controls — the only
 * moving part under test here is 6DOF masking on a single 2D player.
 * gridRes / minBlockCount / numBufferedBlocks are NOT prompted — edit the
 * DEFAULT_6DOF_* constants below directly, as requested.
 */

#include <iostream>
#include <string>
#include <vector>
#include <cmath>
#include <limits>

#include "AT_AudioManager.h"

#ifdef _WIN32
#define CLEAR_CONSOLE "cls"
#else
#define CLEAR_CONSOLE "clear"
#endif

constexpr float PI = 3.14159265358979323846f;

// ── 6DOF parameters — edit here directly, not prompted (per request) ───────
// gridRes=0.02 / minBlockCount=4: validated on the clean synthetic corpus.
// numBufferedBlocks=1: try first (lowest latency, one full audio block).
constexpr float DEFAULT_6DOF_GRID_RES           = 0.02f;
constexpr int   DEFAULT_6DOF_MIN_BLOCK_COUNT    = 4;
constexpr int   DEFAULT_6DOF_NUM_BUFFERED_BLOCKS = 6;

constexpr int DEFAULT_BUFFER_SIZE = 4096;
constexpr int DEFAULT_SAMPLE_RATE = 48000;

std::unique_ptr<AT::AudioManager> g_audioManager;

struct AppConfig
{
    std::string audioFilePath;
    std::string selectedDeviceName;
    int   numVirtualSpeakers = 80;
    float speakerCircleRadius = 2.0f;
    int   playerUID = -1;

    float listenerPosX = 0.0f;
    float listenerPosY = 0.0f;
    float listenerPosZ = 0.0f;
};

AppConfig g_config;

// ============================================================================
// UTILITIES
// ============================================================================

void clearConsole() { system(CLEAR_CONSOLE); }

void waitForEnter()
{
    std::cout << "\nPress ENTER to continue...";
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    std::cin.get();
}

void printSeparator() { std::cout << "\n========================================\n"; }

// ============================================================================
// SETUP PROMPTS
// ============================================================================

std::string promptAudioDevice()
{
    std::cout << "\n=== AUDIO DEVICE SELECTION ===\n";
    std::cout << "Scanning available audio devices...\n";
#if JUCE_WINDOWS
    g_audioManager->refreshDevices(true, false);
#else
    g_audioManager->refreshDevices(false, false);
#endif

    int deviceCount = g_audioManager->getCachedDeviceCount();
    for (int i = 0; i < deviceCount; i++)
        g_audioManager->getDetailedDeviceInfo(i);

    if (deviceCount == 0)
    {
        std::cout << "No audio devices found — using system default.\n";
        return "";
    }

    std::cout << "\nAvailable audio devices:\n";
    for (int i = 0; i < deviceCount; i++)
    {
        AT::DeviceInfo device = g_audioManager->getCachedDeviceInfo(i);
        if (device.maxOutputChannels > 0)
            std::cout << "[" << i << "] " << device.name << " (" << device.typeName
                       << ") — Outputs: " << device.maxOutputChannels << "\n";
    }

    int selection;
    std::cout << "\nEnter device index (or -1 for default): ";
    std::cin >> selection;
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

    if (selection < 0 || selection >= deviceCount)
    {
        std::cout << "Using system default device.\n";
        return "";
    }

    AT::DeviceInfo selected = g_audioManager->getCachedDeviceInfo(selection);
    std::cout << "Selected: " << selected.name << "\n";
    return selected.name;
}

int promptNumVirtualSpeakers()
{
    int n;
    std::cout << "\n=== VIRTUAL SPEAKER COUNT ===\n";
    std::cout << "This must match the channel count of the 2D audio file\n";
    std::cout << "(e.g. 80 for an equator/tropic ring export).\n> ";
    std::cin >> n;
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    if (n < 2) { std::cout << "Warning: minimum 2, using 2.\n"; n = 2; }
    return n;
}

float promptSpeakerDiameter()
{
    float diameter;
    std::cout << "\n=== SPEAKER CIRCLE DIAMETER (m) ===\n> ";
    std::cin >> diameter;
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    float radius = diameter / 2.0f;
    if (radius < 0.1f) { std::cout << "Warning: too small, using 0.5m radius.\n"; radius = 0.5f; }
    return radius;
}

std::string promptAudioFilePath()
{
    std::string path;
    std::cout << "\n=== 2D AUDIO FILE (6DOF test source) ===\n";
    std::cout << "Full path to the multichannel .wav file:\n> ";
    std::getline(std::cin, path);
    return path;
}

// ============================================================================
// SCENE SETUP
// ============================================================================

void setupVirtualSpeakers()
{
    std::cout << "\nConfiguring " << g_config.numVirtualSpeakers
               << " virtual speakers, radius " << g_config.speakerCircleRadius << "m...\n";

    int n = g_config.numVirtualSpeakers;
    std::vector<float> positions(n * 3), rotations(n * 3), forwards(n * 3);

    // MUST match At_SpeakerConfig.circleConfig() (Unity) and simulate_capture.py
    // EXACTLY: angle starts at -PI/2, x=R*sin, z=R*cos — NOT angle starting at
    // 0 (the previous formula here). Unlike a sin/cos swap (a mirror), this
    // specific mismatch is a pure 90-degree ROTATION — which is far more
    // insidious: it produces a PERFECTLY CLEAN least-squares fit (near-zero
    // residual, indistinguishable from a genuine correct localization) but a
    // systematically wrong, rotated position. Confirmed by directly
    // reproducing two "phantom" positions seen in real 6DOF logs — both
    // matched EXACTLY the true source positions rotated -90 degrees.
    const float angularStep = 2.0f * PI / n;
    for (int i = 0; i < n; i++)
    {
        float angle = -PI / 2.0f + i * angularStep;
        positions[i * 3 + 0] = g_config.speakerCircleRadius * std::sin(angle);
        positions[i * 3 + 1] = 0.0f;
        positions[i * 3 + 2] = g_config.speakerCircleRadius * std::cos(angle);

        rotations[i * 3 + 0] = 0.0f;
        rotations[i * 3 + 1] = angle * 180.0f / PI;
        rotations[i * 3 + 2] = 0.0f;

        forwards[i * 3 + 0] = -std::sin(angle);
        forwards[i * 3 + 1] = 0.0f;
        forwards[i * 3 + 2] = -std::cos(angle);
    }

    g_audioManager->setVirtualSpeakerTransform(positions.data(), rotations.data(), forwards.data(), n);
    std::cout << "Virtual speakers configured.\n";
}

void updateListenerTransform()
{
    float position[3] = { g_config.listenerPosX, g_config.listenerPosY, g_config.listenerPosZ };
    float rotation[3] = { 0.0f, 0.0f, 0.0f };
    float forward[3]  = { 0.0f, 0.0f, 1.0f };  // fixed forward — only position matters for masking
    g_audioManager->setListenerTransform(position, rotation, forward);
}

bool initializeAudioSystem()
{
    printSeparator();
    std::cout << "=== 2D + 6DOF MASKING TEST ===\n";
    printSeparator();

    g_config.numVirtualSpeakers  = promptNumVirtualSpeakers();
    g_config.speakerCircleRadius = promptSpeakerDiameter();
    g_config.audioFilePath       = promptAudioFilePath();
    if (g_config.audioFilePath.empty())
    {
        std::cout << "Error: no file path provided.\n";
        return false;
    }
    g_config.selectedDeviceName = promptAudioDevice();

    printSeparator();
    std::cout << "\n=== CONFIGURATION SUMMARY ===\n";
    std::cout << "Audio file:       " << g_config.audioFilePath << "\n";
    std::cout << "Virtual speakers: " << g_config.numVirtualSpeakers << "\n";
    std::cout << "Speaker radius:   " << g_config.speakerCircleRadius << "m\n";
    std::cout << "Device:           " << (g_config.selectedDeviceName.empty() ? "DEFAULT" : g_config.selectedDeviceName) << "\n";
    std::cout << "Buffer size:      " << DEFAULT_BUFFER_SIZE << " samples\n";
    std::cout << "Sample rate:      " << DEFAULT_SAMPLE_RATE << " Hz\n";
    std::cout << "Stereo downmix:   Amplitude panning (no HRTF)\n";
    std::cout << "6DOF grid_res:    " << DEFAULT_6DOF_GRID_RES << "\n";
    std::cout << "6DOF min_block:   " << DEFAULT_6DOF_MIN_BLOCK_COUNT << "\n";
    std::cout << "6DOF buffered:    " << DEFAULT_6DOF_NUM_BUFFERED_BLOCKS << " block(s)\n";
    printSeparator();
    waitForEnter();

    std::cout << "\nInitializing audio engine...\n";
    if (!g_audioManager->setup(g_config.selectedDeviceName, 0, g_config.numVirtualSpeakers,
                                DEFAULT_BUFFER_SIZE, /*isBinauralVirtualization=*/true))
    {
        std::cout << "Error: engine setup failed (device channel count?).\n";
        return false;
    }

    // Stereo downmix, amplitude-panning mode (no HRTF, no convolution) — the
    // simplified path added specifically to rule out the HRTF/convolution
    // chain while debugging 6DOF masking.
    g_audioManager->setBinauralRenderMode(0);

    setupVirtualSpeakers();

    updateListenerTransform();
    std::cout << "Listener at (0, 0, 0).\n";

    std::cout << "\nCreating 2D player...\n";
    g_audioManager->addPlayer(&g_config.playerUID, /*is3D=*/false, /*isLooping=*/true);
    if (g_config.playerUID < 0)
    {
        std::cout << "Error: failed to create player.\n";
        return false;
    }
    std::cout << "Player created (UID: " << g_config.playerUID << ")\n";

    if (!g_audioManager->setPlayerFilePath(g_config.playerUID, g_config.audioFilePath.c_str()))
    {
        std::cout << "Error: failed to load audio file — check channel count matches "
                     "virtual speaker count exactly.\n";
        return false;
    }
    std::cout << "Audio file loaded.\n";

    std::cout << "\nEnabling 6DOF source masking...\n";
    g_audioManager->setPlayer6dofMaskEnabled(g_config.playerUID, true);
    g_audioManager->setPlayer6dofGridRes(g_config.playerUID, DEFAULT_6DOF_GRID_RES);
    g_audioManager->setPlayer6dofMinBlockCount(g_config.playerUID, DEFAULT_6DOF_MIN_BLOCK_COUNT);
    g_audioManager->setPlayer6dofNumBufferedBlocks(g_config.playerUID, DEFAULT_6DOF_NUM_BUFFERED_BLOCKS);
    std::cout << "6DOF masking enabled.\n";

    std::cout << "\nStarting playback (looping)...\n";
    g_audioManager->startPlayer(g_config.playerUID);

    std::cout << "\n=> Audio system ready.\n";
    return true;
}

// ============================================================================
// INTERACTIVE LOOP
// ============================================================================

void handleListenerPositionChange()
{
    std::cout << "\n=== CHANGE LISTENER POSITION ===\n";
    std::cout << "Current: (" << g_config.listenerPosX << ", "
               << g_config.listenerPosY << ", " << g_config.listenerPosZ << ")\n";
    std::cout << "New X: "; std::cin >> g_config.listenerPosX;
    std::cout << "New Y: "; std::cin >> g_config.listenerPosY;
    std::cout << "New Z: "; std::cin >> g_config.listenerPosZ;
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

    updateListenerTransform();
    std::cout << "Listener moved to (" << g_config.listenerPosX << ", "
               << g_config.listenerPosY << ", " << g_config.listenerPosZ << ")\n";
}

void displayStatus()
{
    printSeparator();
    std::cout << "STATUS\n";
    std::cout << "  Listener: (" << g_config.listenerPosX << ", "
               << g_config.listenerPosY << ", " << g_config.listenerPosZ << ")\n";

    int numSources = g_audioManager->getPlayer6dofNumDetectedSources(g_config.playerUID);
    std::cout << "  6DOF detected sources: " << numSources << "\n";
    if (numSources > 0)
    {
        std::vector<float> positions(numSources * 3);
        g_audioManager->getPlayer6dofSourcePositions(g_config.playerUID, positions.data(), numSources);
        for (int i = 0; i < numSources; i++)
        {
            std::cout << "    S" << i << " = (" << positions[i * 3 + 0] << ", "
                       << positions[i * 3 + 1] << ", " << positions[i * 3 + 2] << ")\n";
        }
    }
    printSeparator();
}

void showMenu()
{
    std::cout << "\n=== CONTROLS ===\n";
    std::cout << "[l] Change listener position (X, Y, Z)\n";
    std::cout << "[i] Show status (listener + detected 6DOF sources)\n";
    std::cout << "[q] Quit\n> ";
}

void interactiveLoop()
{
    displayStatus();
    char command;
    bool running = true;
    while (running)
    {
        showMenu();
        std::cin >> command;
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

        switch (command)
        {
            case 'l': case 'L': handleListenerPositionChange(); break;
            case 'i': case 'I': displayStatus(); break;
            case 'q': case 'Q': std::cout << "\nExiting...\n"; running = false; break;
            default: std::cout << "Unknown command.\n"; break;
        }
    }
}

// ============================================================================
// MAIN
// ============================================================================

int main(int argc, char* argv[])
{
    std::cout << "========================================\n";
    std::cout << "  2D + 6DOF MASKING TEST APPLICATION\n";
    std::cout << "========================================\n";
    waitForEnter();

    g_audioManager = std::make_unique<AT::AudioManager>();

    if (!initializeAudioSystem())
    {
        std::cout << "\nFailed to initialize audio system.\n";
        waitForEnter();
        return 1;
    }

    interactiveLoop();

    std::cout << "\nStopping playback...\n";
    g_audioManager->stopPlayer(g_config.playerUID);
    g_audioManager->removePlayer(g_config.playerUID);
    g_audioManager->stop();

    // Explicit destruction before main() exits — avoids a ShutdownDetector
    // leak from JUCE statics being torn down after AudioManager.
    g_audioManager.reset();

    std::cout << "\nGoodbye!\n";
    return 0;
}
