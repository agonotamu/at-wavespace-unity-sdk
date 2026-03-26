/**
 * @file UnityLogger.cpp
 * @brief Display logs in Unity Console
 * Use in cpp files :
 * LOG("Spat Engine created!");
 * LOG("Number of channels: " << m_numOutputChannels);
 * Warnings:
 * LOG_WARNING("Buffer size is suboptimal");
 * Errors :
 * LOG_ERROR("Failed to load audio file");
 * @author Antoine Gonot
 * @date 2025
 */

#include "UnityLogger.h"


UnityLogger& UnityLogger::getInstance()
{
    static UnityLogger instance;
    return instance;
}

void UnityLogger::setCallback(UnityLogCallback callback)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_callback = callback;
}

void UnityLogger::log(const std::string& message)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (m_callback != nullptr)
    {
        std::string formatted = "[SPAT] " + message;
        m_callback(formatted.c_str());
    }
}

void UnityLogger::logWarning(const std::string& message)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (m_callback != nullptr)
    {
        std::string formatted = "[SPAT WARNING] " + message;
        m_callback(formatted.c_str());
    }
}

void UnityLogger::logError(const std::string& message)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (m_callback != nullptr)
    {
        std::string formatted = "[SPAT ERROR] " + message;
        m_callback(formatted.c_str());
    }
}
