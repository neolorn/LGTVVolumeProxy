#pragma once

#include "framework.h"
#include <winhttp.h>
#include "Configuration.h"
#include <string>

// Client used to control an LG webOS TV over WebSockets.
class LGWebOSClient
{
public:
    LGWebOSClient();

    // Sets the configuration that supplies TV IP, MAC and port information.
    void SetConfiguration(const AppConfiguration* configuration);

    // Sends a volume up command to the TV.
    bool VolumeUp();

    // Sends a volume down command to the TV.
    bool VolumeDown();

    // Toggles mute state on the TV.
    bool ToggleMute();

    // Performs explicit pairing with the TV, showing any prompts on the given window.
    bool PairWithTv(HWND parentWindow);

    // Removes the stored client key so the next operation requires pairing again.
    bool UnpairFromTv();

    // Returns true when a client key is present on disk.
    bool HasClientKey() const;

    // Sets the TV volume to a specific level.
    bool SetVolume(int volumeLevel);

    // Sets the TV mute state explicitly.
    bool SetMute(bool mute);

private:
    bool SendSimpleCommand(const char* uri);
    bool SendCommandWithPayload(const char* uri, const char* payload);
    bool Connect(HINTERNET& webSocketHandle);
    bool SendRegister(HINTERNET webSocketHandle, const std::string& clientKey);
    bool SendText(HINTERNET webSocketHandle, const std::string& text);
    bool ReceiveOneTextMessage(HINTERNET webSocketHandle, std::string& outMessage);
    void CloseWebSocket(HINTERNET webSocketHandle);

    std::string BuildRegisterMessage(const std::string& clientKey);
    std::string BuildRequestMessage(const char* uri, const char* payloadOrNull);

    std::string GetClientKeyPath() const;
    std::string LoadClientKey() const;
    void SaveClientKey(const std::string& key) const;
    bool DeleteClientKey() const;

    std::string ParseClientKey(const std::string& json) const;
    bool ParseMutedFlag(const std::string& json, bool& muted) const;

    bool VerifyMacAddressMatchesConfiguration(bool showUserError);

    const AppConfiguration* configuration;
    std::wstring lastVerifiedIpAddress;
    std::wstring lastVerifiedMacAddress;
    bool lastMacVerificationResult;
};

// Returns the global LG webOS client instance.
LGWebOSClient& GetTVClient();

// Initializes the global client with the current configuration.
void InitializeTVClient(const AppConfiguration* configuration);
