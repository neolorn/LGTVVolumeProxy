#pragma once

#include <string>

// Represents application configuration persisted on disk.
struct AppConfiguration
{
    std::wstring tvIpAddress;
    std::wstring tvMacAddress;
    std::wstring deviceNameHint;
    bool onlyWhenDolbyAtmos;
    bool useSecureWebSocket;
    unsigned short tvPort;
    bool showCloseToTrayMessage;
    int windowLeft;
    int windowTop;
    bool hasWindowPosition;

    AppConfiguration();
};

// Returns the full path to the configuration file.
std::wstring GetConfigurationFilePath();

// Loads configuration from disk if present and leaves defaults otherwise.
void LoadConfiguration(AppConfiguration& configuration);

// Saves configuration to disk.
void SaveConfiguration(const AppConfiguration& configuration);
