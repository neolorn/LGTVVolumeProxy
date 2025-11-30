#include "TVClient.h"

#include "Logging.h"

#include <winhttp.h>
#include <iphlpapi.h>
#include <ws2tcpip.h>

#include <cstdio>
#include <fstream>

#pragma comment(lib, "Winhttp.lib")
#pragma comment(lib, "Iphlpapi.lib")
#pragma comment(lib, "Ws2_32.lib")

namespace
{
    std::string WideToUtf8(const std::wstring& value)
    {
        if (value.empty())
        {
            return {};
        }

        int requiredLength = WideCharToMultiByte(
            CP_UTF8,
            0,
            value.c_str(),
            static_cast<int>(value.size()),
            nullptr,
            0,
            nullptr,
            nullptr);
        if (requiredLength <= 0)
        {
            return {};
        }

        std::string result(static_cast<size_t>(requiredLength), '\0');
        int written = WideCharToMultiByte(
            CP_UTF8,
            0,
            value.c_str(),
            static_cast<int>(value.size()),
            result.data(),
            requiredLength,
            nullptr,
            nullptr);
        if (written != requiredLength)
        {
            result.clear();
        }

        return result;
    }

    bool ResolveMacForIp(const std::wstring& ipAddress, std::wstring& macAddressOut)
    {
        SOCKADDR_IN ipv4Address{};
        ipv4Address.sin_family = AF_INET;

        int inetResult = InetPtonW(
            AF_INET,
            ipAddress.c_str(),
            &ipv4Address.sin_addr);
        if (inetResult != 1)
        {
            ErrorLog(L"[LGTV] InetPtonW failed for IP '%s'", ipAddress.c_str());
            return false;
        }

        ULONG macAddressBuffer[2]{};
        ULONG physicalAddressLength = 6;

        DWORD arpResult = SendARP(
            ipv4Address.sin_addr.S_un.S_addr,
            0,
            macAddressBuffer,
            &physicalAddressLength);
        if (arpResult != NO_ERROR || physicalAddressLength < 6)
        {
            ErrorLog(L"[LGTV] SendARP failed for IP '%s', result=%lu", ipAddress.c_str(), arpResult);
            return false;
        }

        const BYTE* macBytes = reinterpret_cast<const BYTE*>(macAddressBuffer);
        wchar_t formatted[32]{};
        swprintf_s(
            formatted,
            L"%02X:%02X:%02X:%02X:%02X:%02X",
            macBytes[0],
            macBytes[1],
            macBytes[2],
            macBytes[3],
            macBytes[4],
            macBytes[5]);

        macAddressOut.assign(formatted);
        return true;
    }

    std::wstring NormalizeMacString(const std::wstring& macAddress)
    {
        std::wstring normalized;
        normalized.reserve(macAddress.size());

        for (wchar_t character : macAddress)
        {
            if (iswxdigit(character))
            {
                normalized.push_back(static_cast<wchar_t>(towupper(character)));
            }
        }

        return normalized;
    }

    LGWebOSClient g_globalTVClient;
}

LGWebOSClient::LGWebOSClient()
    : configuration(nullptr),
    lastVerifiedIpAddress(),
    lastVerifiedMacAddress(),
    lastMacVerificationResult(false)
{
}

void LGWebOSClient::SetConfiguration(const AppConfiguration* configurationValue)
{
    configuration = configurationValue;
    lastVerifiedIpAddress.clear();
    lastVerifiedMacAddress.clear();
    lastMacVerificationResult = false;
}

bool LGWebOSClient::VolumeUp()
{
    return SendSimpleCommand("ssap://audio/volumeUp");
}

bool LGWebOSClient::VolumeDown()
{
    return SendSimpleCommand("ssap://audio/volumeDown");
}

bool LGWebOSClient::ToggleMute()
{
    std::string clientKey = LoadClientKey();
    if (clientKey.empty())
    {
        DebugLog(L"[LGTV] ToggleMute: no client key yet (not paired)");
        return false;
    }

    HINTERNET webSocketHandle = nullptr;
    if (!Connect(webSocketHandle))
    {
        return false;
    }

    if (!SendRegister(webSocketHandle, clientKey))
    {
        DebugLog(L"[LGTV] ToggleMute: SendRegister failed");
        CloseWebSocket(webSocketHandle);
        return false;
    }

    std::string temporary;
    ReceiveOneTextMessage(webSocketHandle, temporary);

    std::string getStatusRequest = BuildRequestMessage("ssap://audio/getStatus", nullptr);
    if (!SendText(webSocketHandle, getStatusRequest))
    {
        DebugLog(L"[LGTV] ToggleMute: send getStatus failed");
        CloseWebSocket(webSocketHandle);
        return false;
    }

    std::string statusResponse;
    if (!ReceiveOneTextMessage(webSocketHandle, statusResponse))
    {
        DebugLog(L"[LGTV] ToggleMute: receive getStatus failed");
        CloseWebSocket(webSocketHandle);
        return false;
    }

    bool muted = false;
    if (!ParseMutedFlag(statusResponse, muted))
    {
        DebugLog(L"[LGTV] ToggleMute: failed to parse muted flag, forcing mute=true");
        std::string payload = "{\"mute\":true}";
        std::string setMuteRequest = BuildRequestMessage("ssap://audio/setMute", payload.c_str());
        SendText(webSocketHandle, setMuteRequest);
        CloseWebSocket(webSocketHandle);
        return true;
    }

    bool newMuted = !muted;
    std::string payload =
        std::string("{\"mute\":") + (newMuted ? "true" : "false") + "}";
    std::string setMuteRequest =
        BuildRequestMessage("ssap://audio/setMute", payload.c_str());
    SendText(webSocketHandle, setMuteRequest);

    CloseWebSocket(webSocketHandle);
    return true;
}

bool LGWebOSClient::PairWithTv(HWND parentWindow)
{
    DebugLog(L"[LGTV] PairWithTv: starting");

    if (!VerifyMacAddressMatchesConfiguration(true))
    {
        return false;
    }

    HINTERNET webSocketHandle = nullptr;
    if (!Connect(webSocketHandle))
    {
        DebugLog(L"[LGTV] PairWithTv: Connect() failed");
        return false;
    }

    std::string emptyKey;
    if (!SendRegister(webSocketHandle, emptyKey))
    {
        DebugLog(L"[LGTV] PairWithTv: SendRegister() failed");
        CloseWebSocket(webSocketHandle);
        return false;
    }

    DebugLog(L"[LGTV] PairWithTv: register sent, TV should show PROMPT now");

    MessageBoxW(
        parentWindow,
        L"Check your LG TV and ACCEPT the pairing prompt.\n\n"
        L"After accepting on the TV, click OK here to finish pairing.",
        L"LG TV Volume Proxy - Pairing",
        MB_OK | MB_ICONINFORMATION);

    DebugLog(L"[LGTV] PairWithTv: waiting for response(s) with client-key");

    std::string response;
    std::string newKey;

    for (int index = 0; index < 5; ++index)
    {
        response.clear();
        if (!ReceiveOneTextMessage(webSocketHandle, response))
        {
            DebugLog(L"[LGTV] PairWithTv: ReceiveOneTextMessage() failed on iteration %d", index);
            break;
        }

        std::wstring wideResponse(response.begin(), response.end());
        if (wideResponse.size() > 400)
        {
            wideResponse.resize(400);
        }
        DebugLog(L"[LGTV] PairWithTv: RECV[%d]: %s", index, wideResponse.c_str());

        newKey = ParseClientKey(response);
        if (!newKey.empty())
        {
            DebugLog(L"[LGTV] PairWithTv: found client-key in RECV[%d]", index);
            break;
        }
    }

    if (newKey.empty())
    {
        DebugLog(L"[LGTV] PairWithTv: no client-key found in any response");
        CloseWebSocket(webSocketHandle);
        return false;
    }

    SaveClientKey(newKey);
    DebugLog(L"[LGTV] PairWithTv: stored client-key (***hidden***)");

    CloseWebSocket(webSocketHandle);
    return true;
}

bool LGWebOSClient::UnpairFromTv()
{
    if (!HasClientKey())
    {
        DebugLog(L"[LGTV] UnpairFromTv: no client key present");
        return true;
    }

    if (!DeleteClientKey())
    {
        WarningLog(L"[LGTV] UnpairFromTv: client key file delete failed");
        return false;
    }

    DebugLog(L"[LGTV] UnpairFromTv: client key removed");
    return true;
}

bool LGWebOSClient::HasClientKey() const
{
    return !LoadClientKey().empty();
}

bool LGWebOSClient::SetVolume(int volumeLevel)
{
    if (volumeLevel < 0)
    {
        volumeLevel = 0;
    }

    std::string payload = std::string("{\"volume\":") + std::to_string(volumeLevel) + "}";
    return SendCommandWithPayload("ssap://audio/setVolume", payload.c_str());
}

bool LGWebOSClient::SetMute(bool mute)
{
    std::string payload =
        std::string("{\"mute\":") + (mute ? "true" : "false") + "}";
    return SendCommandWithPayload("ssap://audio/setMute", payload.c_str());
}

bool LGWebOSClient::SendSimpleCommand(const char* uri)
{
    std::string clientKey = LoadClientKey();
    if (clientKey.empty())
    {
        DebugLog(L"[LGTV] SendSimpleCommand: no client key yet (not paired)");
        return false;
    }

    HINTERNET webSocketHandle = nullptr;
    if (!Connect(webSocketHandle))
    {
        return false;
    }

    if (!SendRegister(webSocketHandle, clientKey))
    {
        DebugLog(L"[LGTV] SendSimpleCommand: SendRegister failed");
        CloseWebSocket(webSocketHandle);
        return false;
    }

    std::string acknowledge;
    ReceiveOneTextMessage(webSocketHandle, acknowledge);

    std::string request = BuildRequestMessage(uri, nullptr);
    SendText(webSocketHandle, request);

    CloseWebSocket(webSocketHandle);
    return true;
}

bool LGWebOSClient::SendCommandWithPayload(const char* uri, const char* payload)
{
    std::string clientKey = LoadClientKey();
    if (clientKey.empty())
    {
        DebugLog(L"[LGTV] SendCommandWithPayload: no client key yet (not paired)");
        return false;
    }

    HINTERNET webSocketHandle = nullptr;
    if (!Connect(webSocketHandle))
    {
        return false;
    }

    if (!SendRegister(webSocketHandle, clientKey))
    {
        DebugLog(L"[LGTV] SendCommandWithPayload: SendRegister failed");
        CloseWebSocket(webSocketHandle);
        return false;
    }

    std::string acknowledge;
    ReceiveOneTextMessage(webSocketHandle, acknowledge);

    std::string request = BuildRequestMessage(uri, payload);
    SendText(webSocketHandle, request);

    CloseWebSocket(webSocketHandle);
    return true;
}

bool LGWebOSClient::Connect(HINTERNET& webSocketHandle)
{
    webSocketHandle = nullptr;

    if (!configuration)
    {
        ErrorLog(L"[LGTV] Connect: configuration not set");
        return false;
    }

    if (configuration->tvIpAddress.empty())
    {
        ErrorLog(L"[LGTV] Connect: no TV IP configured");
        return false;
    }

    if (!VerifyMacAddressMatchesConfiguration(false))
    {
        ErrorLog(L"[LGTV] Connect: MAC verification failed");
        return false;
    }

    HINTERNET sessionHandle = WinHttpOpen(
        L"LGTVVolumeProxy/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);
    if (!sessionHandle)
    {
        ErrorLog(L"[LGTV] WinHttpOpen failed: %lu", GetLastError());
        return false;
    }

    HINTERNET connectHandle = WinHttpConnect(
        sessionHandle,
        configuration->tvIpAddress.c_str(),
        configuration->tvPort,
        0);
    if (!connectHandle)
    {
        ErrorLog(L"[LGTV] WinHttpConnect failed: %lu", GetLastError());
        WinHttpCloseHandle(sessionHandle);
        return false;
    }

    DWORD flags = configuration->useSecureWebSocket ? WINHTTP_FLAG_SECURE : 0;

    HINTERNET requestHandle = WinHttpOpenRequest(
        connectHandle,
        L"GET",
        L"/",
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        flags);
    if (!requestHandle)
    {
        ErrorLog(L"[LGTV] WinHttpOpenRequest failed: %lu", GetLastError());
        WinHttpCloseHandle(connectHandle);
        WinHttpCloseHandle(sessionHandle);
        return false;
    }

    if (configuration->useSecureWebSocket)
    {
        DWORD securityFlags =
            SECURITY_FLAG_IGNORE_UNKNOWN_CA |
            SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
            SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
            SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;

        if (!WinHttpSetOption(
            requestHandle,
            WINHTTP_OPTION_SECURITY_FLAGS,
            &securityFlags,
            sizeof(securityFlags)))
        {
            WarningLog(L"[LGTV] WinHttpSetOption(SECURITY_FLAGS) failed: %lu", GetLastError());
        }
    }

    #pragma warning(suppress:6387)
    if (!WinHttpSetOption(
        requestHandle,
        WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET,
        nullptr,
        0))
    {
        ErrorLog(L"[LGTV] WinHttpSetOption(UPGRADE_TO_WEB_SOCKET) failed: %lu", GetLastError());
        WinHttpCloseHandle(requestHandle);
        WinHttpCloseHandle(connectHandle);
        WinHttpCloseHandle(sessionHandle);
        return false;
    }

    BOOL sendResult = WinHttpSendRequest(
        requestHandle,
        WINHTTP_NO_ADDITIONAL_HEADERS,
        0,
        WINHTTP_NO_REQUEST_DATA,
        0,
        0,
        0);
    if (!sendResult)
    {
        ErrorLog(L"[LGTV] WinHttpSendRequest failed: %lu", GetLastError());
        WinHttpCloseHandle(requestHandle);
        WinHttpCloseHandle(connectHandle);
        WinHttpCloseHandle(sessionHandle);
        return false;
    }

    BOOL receiveResult = WinHttpReceiveResponse(requestHandle, nullptr);
    if (!receiveResult)
    {
        ErrorLog(L"[LGTV] WinHttpReceiveResponse failed: %lu", GetLastError());
        WinHttpCloseHandle(requestHandle);
        WinHttpCloseHandle(connectHandle);
        WinHttpCloseHandle(sessionHandle);
        return false;
    }

    DWORD statusCode = 0;
    DWORD statusCodeSize = sizeof(statusCode);
    if (!WinHttpQueryHeaders(
        requestHandle,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX,
        &statusCode,
        &statusCodeSize,
        WINHTTP_NO_HEADER_INDEX))
    {
        ErrorLog(L"[LGTV] WinHttpQueryHeaders failed: %lu", GetLastError());
        WinHttpCloseHandle(requestHandle);
        WinHttpCloseHandle(connectHandle);
        WinHttpCloseHandle(sessionHandle);
        return false;
    }

    if (statusCode != 101)
    {
        ErrorLog(L"[LGTV] WebSocket upgrade failed, HTTP status = %lu", statusCode);
        WinHttpCloseHandle(requestHandle);
        WinHttpCloseHandle(connectHandle);
        WinHttpCloseHandle(sessionHandle);
        return false;
    }

    HINTERNET webSocket = WinHttpWebSocketCompleteUpgrade(requestHandle, 0);
    if (!webSocket)
    {
        ErrorLog(L"[LGTV] WinHttpWebSocketCompleteUpgrade failed: %lu", GetLastError());
        WinHttpCloseHandle(requestHandle);
        WinHttpCloseHandle(connectHandle);
        WinHttpCloseHandle(sessionHandle);
        return false;
    }

    WinHttpCloseHandle(requestHandle);
    WinHttpCloseHandle(connectHandle);
    WinHttpCloseHandle(sessionHandle);

    webSocketHandle = webSocket;
    return true;
}

bool LGWebOSClient::SendRegister(HINTERNET webSocketHandle, const std::string& clientKey)
{
    std::string message = BuildRegisterMessage(clientKey);
    return SendText(webSocketHandle, message);
}

bool LGWebOSClient::SendText(HINTERNET webSocketHandle, const std::string& text)
{
    if (!webSocketHandle)
    {
        return false;
    }

    HRESULT result = WinHttpWebSocketSend(
        webSocketHandle,
        WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
        reinterpret_cast<BYTE*>(const_cast<char*>(text.data())),
        static_cast<DWORD>(text.size()));
    if (FAILED(result))
    {
        ErrorLog(L"[LGTV] WinHttpWebSocketSend failed: 0x%08X", result);
        return false;
    }
    return true;
}

bool LGWebOSClient::ReceiveOneTextMessage(HINTERNET webSocketHandle, std::string& outMessage)
{
    if (!webSocketHandle)
    {
        return false;
    }

    BYTE buffer[4096];
    DWORD bytesRead = 0;
    WINHTTP_WEB_SOCKET_BUFFER_TYPE bufferType =
        WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE;

    HRESULT result = WinHttpWebSocketReceive(
        webSocketHandle,
        buffer,
        sizeof(buffer),
        &bytesRead,
        &bufferType);
    if (FAILED(result))
    {
        ErrorLog(L"[LGTV] WinHttpWebSocketReceive failed: 0x%08X", result);
        return false;
    }

    if (bufferType == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE)
    {
        DebugLog(L"[LGTV] WebSocket close frame received");
        return false;
    }

    if (bufferType != WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE &&
        bufferType != WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE)
    {
        ErrorLog(L"[LGTV] Unexpected buffer type: %d", static_cast<int>(bufferType));
        return false;
    }

    outMessage.assign(
        reinterpret_cast<char*>(buffer),
        reinterpret_cast<char*>(buffer) + bytesRead);
    return true;
}

void LGWebOSClient::CloseWebSocket(HINTERNET webSocketHandle)
{
    if (!webSocketHandle)
    {
        return;
    }

    WinHttpWebSocketShutdown(
        webSocketHandle,
        WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS,
        nullptr,
        0);
    WinHttpCloseHandle(webSocketHandle);
}

std::string LGWebOSClient::BuildRegisterMessage(const std::string& clientKey)
{
    std::string message;
    message.reserve(512);

    message += "{";
    message += "\"type\":\"register\",";
    message += "\"id\":\"register_0\",";
    message += "\"payload\":{";
    message += "\"forcePairing\":false,";
    message += "\"pairingType\":\"PROMPT\",";
    if (!clientKey.empty())
    {
        message += "\"client-key\":\"";
        message += clientKey;
        message += "\",";
    }
    message += "\"manifest\":{";
    message += "\"manifestVersion\":1,";
    message += "\"appVersion\":\"1.0\",";
    message += "\"appId\":\"com.lgtvvolumeproxy\",";
    message += "\"vendorId\":\"com.lgtvvolumeproxy\",";
    message += "\"localizedAppNames\":{\"\":\"LGTV Volume Proxy\"},";
    message += "\"localizedVendorNames\":{\"\":\"LGTV Volume Proxy\"},";
    message += "\"permissions\":[";
    message += "\"CONTROL_AUDIO\"";
    message += "]";
    message += "}";
    message += "}";
    message += "}";
    return message;
}

std::string LGWebOSClient::BuildRequestMessage(const char* uri, const char* payloadOrNull)
{
    std::string message;
    message.reserve(256);

    message += "{";
    message += "\"type\":\"request\",";
    message += "\"id\":\"req_0\",";
    message += "\"uri\":\"";
    message += uri;
    message += "\"";
    if (payloadOrNull)
    {
        message += ",\"payload\":";
        message += payloadOrNull;
    }
    message += "}";
    return message;
}

std::string LGWebOSClient::GetClientKeyPath() const
{
    std::wstring configPath = GetConfigurationFilePath();
    std::wstring basePath = configPath;
    size_t position = basePath.find_last_of(L".");
    if (position != std::wstring::npos)
    {
        basePath = basePath.substr(0, position);
    }
    basePath += L"_client_key.txt";
    return WideToUtf8(basePath);
}

std::string LGWebOSClient::LoadClientKey() const
{
    std::string path = GetClientKeyPath();
    std::ifstream input(path);
    if (!input)
    {
        return {};
    }

    std::string key;
    std::getline(input, key);
    return key;
}

void LGWebOSClient::SaveClientKey(const std::string& key) const
{
    std::string path = GetClientKeyPath();
    std::ofstream output(path, std::ios::trunc);
    if (!output)
    {
        ErrorLog(L"[LGTV] Failed to write client key file");
        return;
    }
    output << key;
}

bool LGWebOSClient::DeleteClientKey() const
{
    std::string path = GetClientKeyPath();
    if (path.empty())
    {
        return false;
    }

    int result = std::remove(path.c_str());
    return result == 0;
}

std::string LGWebOSClient::ParseClientKey(const std::string& json) const
{
    const std::string token = "\"client-key\"";
    size_t position = json.find(token);
    if (position == std::string::npos)
    {
        return {};
    }

    position = json.find(':', position);
    if (position == std::string::npos)
    {
        return {};
    }

    position = json.find('"', position);
    if (position == std::string::npos)
    {
        return {};
    }

    size_t end = json.find('"', position + 1);
    if (end == std::string::npos)
    {
        return {};
    }

    return json.substr(position + 1, end - position - 1);
}

bool LGWebOSClient::ParseMutedFlag(const std::string& json, bool& muted) const
{
    size_t position = json.find("\"muted\"");
    if (position == std::string::npos)
    {
        return false;
    }

    position = json.find(':', position);
    if (position == std::string::npos)
    {
        return false;
    }

    size_t truePosition = json.find("true", position);
    size_t falsePosition = json.find("false", position);

    if (truePosition != std::string::npos &&
        (falsePosition == std::string::npos || truePosition < falsePosition))
    {
        muted = true;
        return true;
    }
    if (falsePosition != std::string::npos)
    {
        muted = false;
        return true;
    }
    return false;
}

bool LGWebOSClient::VerifyMacAddressMatchesConfiguration(bool showUserError)
{
    if (!configuration)
    {
        ErrorLog(L"[LGTV] MAC verification: configuration not set");
        return false;
    }

    if (configuration->tvIpAddress.empty() || configuration->tvMacAddress.empty())
    {
        ErrorLog(L"[LGTV] MAC verification: TV IP or MAC not configured");
        return false;
    }

    if (!lastVerifiedIpAddress.empty() &&
        lastVerifiedIpAddress == configuration->tvIpAddress &&
        lastVerifiedMacAddress == configuration->tvMacAddress)
    {
        return lastMacVerificationResult;
    }

    std::wstring resolvedMac;
    if (!ResolveMacForIp(configuration->tvIpAddress, resolvedMac))
    {
        ErrorLog(L"[LGTV] MAC verification: failed to resolve MAC for IP %s", configuration->tvIpAddress.c_str());
        lastVerifiedIpAddress = configuration->tvIpAddress;
        lastVerifiedMacAddress = configuration->tvMacAddress;
        lastMacVerificationResult = false;
        if (showUserError)
        {
            MessageBoxW(
                nullptr,
                L"Unable to resolve MAC address for the configured TV IP.\n\n"
                L"Check that the TV is powered on and reachable.",
                L"LG TV Volume Proxy - MAC verification",
                MB_OK | MB_ICONERROR);
        }
        return false;
    }

    std::wstring expected = NormalizeMacString(configuration->tvMacAddress);
    std::wstring actual = NormalizeMacString(resolvedMac);

    bool match = (expected == actual);

    lastVerifiedIpAddress = configuration->tvIpAddress;
    lastVerifiedMacAddress = configuration->tvMacAddress;
    lastMacVerificationResult = match;

    if (!match)
    {
        ErrorLog(
            L"[LGTV] MAC verification failed: configured=%s, actual=%s",
            configuration->tvMacAddress.c_str(),
            resolvedMac.c_str());

        if (showUserError)
        {
            MessageBoxW(
                nullptr,
                L"The configured TV MAC address does not match the device at the configured IP.\n\n"
                L"Update the configuration so both IP and MAC refer to the same TV.",
                L"LG TV Volume Proxy - MAC verification",
                MB_OK | MB_ICONERROR);
        }
    }

    return match;
}

LGWebOSClient& GetTVClient()
{
    return g_globalTVClient;
}

void InitializeTVClient(const AppConfiguration* configuration)
{
    g_globalTVClient.SetConfiguration(configuration);
}
