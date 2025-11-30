#include "Configuration.h"

#include "framework.h"
#include "Logging.h"

#include <cwctype>
#include <fstream>
#include <string>

AppConfiguration::AppConfiguration()
    : tvIpAddress(L""),
    tvMacAddress(L""),
    deviceNameHint(L"LG"),
    onlyWhenDolbyAtmos(true),
    useSecureWebSocket(true),
    tvPort(3001),
    showCloseToTrayMessage(true),
    windowLeft(-1),
    windowTop(-1),
    hasWindowPosition(false)
{
}

std::wstring GetConfigurationFilePath()
{
    wchar_t pathBuffer[MAX_PATH]{};
    DWORD length = GetModuleFileNameW(nullptr, pathBuffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
    {
        return L"LGTVVolumeProxy.ini";
    }

    std::wstring fullPath(pathBuffer);
    size_t lastSeparator = fullPath.find_last_of(L"\\/");
    std::wstring directory =
        (lastSeparator != std::wstring::npos)
        ? fullPath.substr(0, lastSeparator + 1)
        : std::wstring();

    return directory + L"LGTVVolumeProxy.ini";
}

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

    void Trim(std::string& value)
    {
        while (!value.empty()
            && (value.back() == '\r'
                || value.back() == '\n'
                || std::isspace(static_cast<unsigned char>(value.back()))))
        {
            value.pop_back();
        }

        size_t index = 0;
        while (index < value.size()
            && std::isspace(static_cast<unsigned char>(value[index])))
        {
            ++index;
        }

        if (index > 0 && index < value.size())
        {
            value = value.substr(index);
        }
        else if (index >= value.size())
        {
            value.clear();
        }
    }
}

void LoadConfiguration(AppConfiguration& configuration)
{
    std::wstring path = GetConfigurationFilePath();
    std::ifstream input(path);
    if (!input)
    {
        InfoLog(L"[Configuration] No configuration file found, using defaults");
        return;
    }

    std::string line;
    while (std::getline(input, line))
    {
        auto separator = line.find('=');
        if (separator == std::string::npos)
        {
            continue;
        }

        std::string key = line.substr(0, separator);
        std::string value = line.substr(separator + 1);

        Trim(key);
        Trim(value);

        if (key == "tv_ip")
        {
            configuration.tvIpAddress.assign(value.begin(), value.end());
        }
        else if (key == "tv_mac")
        {
            configuration.tvMacAddress.assign(value.begin(), value.end());
        }
        else if (key == "device_hint")
        {
            configuration.deviceNameHint.assign(value.begin(), value.end());
        }
        else if (key == "only_when_atmos")
        {
            configuration.onlyWhenDolbyAtmos = (value == "1"
                || value == "true"
                || value == "True");
        }
        else if (key == "use_secure_websocket")
        {
            configuration.useSecureWebSocket = (value == "1"
                || value == "true"
                || value == "True");
        }
        else if (key == "tv_port")
        {
            try
            {
                int port = std::stoi(value);
                if (port > 0 && port <= 65535)
                {
                    configuration.tvPort = static_cast<unsigned short>(port);
                }
            }
            catch (...)
            {
                WarningLog(L"[Configuration] Failed to parse tv_port");
            }
        }
        else if (key == "show_close_to_tray_message")
        {
            configuration.showCloseToTrayMessage = !(value == "0"
                || value == "false"
                || value == "False");
        }
        else if (key == "window_left")
        {
            try
            {
                configuration.windowLeft = std::stoi(value);
                configuration.hasWindowPosition = true;
            }
            catch (...)
            {
                WarningLog(L"[Configuration] Failed to parse window_left");
            }
        }
        else if (key == "window_top")
        {
            try
            {
                configuration.windowTop = std::stoi(value);
                configuration.hasWindowPosition = true;
            }
            catch (...)
            {
                WarningLog(L"[Configuration] Failed to parse window_top");
            }
        }
    }
}

void SaveConfiguration(const AppConfiguration& configuration)
{
    std::wstring path = GetConfigurationFilePath();
    std::ofstream output(path, std::ios::trunc);
    if (!output)
    {
        ErrorLog(L"[Configuration] Failed to open configuration file for writing");
        return;
    }

    output << "tv_ip=" << WideToUtf8(configuration.tvIpAddress) << "\n";
    output << "tv_mac=" << WideToUtf8(configuration.tvMacAddress) << "\n";
    output << "device_hint=" << WideToUtf8(configuration.deviceNameHint) << "\n";
    output << "only_when_atmos=" << (configuration.onlyWhenDolbyAtmos ? "1" : "0") << "\n";
    output << "use_secure_websocket=" << (configuration.useSecureWebSocket ? "1" : "0") << "\n";
    output << "tv_port=" << configuration.tvPort << "\n";
    output << "show_close_to_tray_message=" << (configuration.showCloseToTrayMessage ? "1" : "0") << "\n";
    output << "window_left=" << configuration.windowLeft << "\n";
    output << "window_top=" << configuration.windowTop << "\n";
}
