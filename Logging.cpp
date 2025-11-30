#include "Logging.h"

#include <cstdarg>
#include <cstdio>
#include <fstream>

namespace
{
    constexpr size_t MaxLogLineLength = 1024;

    std::wstring BuildLogPrefix(const wchar_t* levelTag)
    {
        SYSTEMTIME localTime{};
        GetLocalTime(&localTime);

        wchar_t buffer[64]{};
        swprintf_s(
            buffer,
            L"[%04u-%02u-%02u %02u:%02u:%02u][%s] ",
            static_cast<unsigned int>(localTime.wYear),
            static_cast<unsigned int>(localTime.wMonth),
            static_cast<unsigned int>(localTime.wDay),
            static_cast<unsigned int>(localTime.wHour),
            static_cast<unsigned int>(localTime.wMinute),
            static_cast<unsigned int>(localTime.wSecond),
            levelTag);

        return std::wstring(buffer);
    }

    std::wstring GetLogFilePath()
    {
        wchar_t pathBuffer[MAX_PATH]{};
        DWORD result = GetModuleFileNameW(nullptr, pathBuffer, MAX_PATH);
        if (result == 0 || result >= MAX_PATH)
        {
            return L"LGTVVolumeProxy.log";
        }

        std::wstring fullPath(pathBuffer);
        size_t lastSeparator = fullPath.find_last_of(L"\\/");
        std::wstring directory =
            (lastSeparator != std::wstring::npos)
            ? fullPath.substr(0, lastSeparator + 1)
            : std::wstring();

        return directory + L"LGTVVolumeProxy.log";
    }

    void WriteLogLine(const wchar_t* levelTag, const wchar_t* format, va_list arguments)
    {
        wchar_t messageBuffer[MaxLogLineLength]{};
        _vsnwprintf_s(
            messageBuffer,
            _TRUNCATE,
            format,
            arguments);

        std::wstring finalLine = BuildLogPrefix(levelTag);
        finalLine += messageBuffer;
        finalLine += L"\n";

        OutputDebugStringW(finalLine.c_str());

        static const std::wstring logPath = GetLogFilePath();
        std::wofstream stream(logPath, std::ios::app);
        if (stream)
        {
            stream << finalLine;
        }
    }
}

void DebugLog(const wchar_t* format, ...)
{
#ifdef _DEBUG
    va_list arguments;
    va_start(arguments, format);
    WriteLogLine(L"DEBUG", format, arguments);
    va_end(arguments);
#else
    (void)format;
#endif
}

void InfoLog(const wchar_t* format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    WriteLogLine(L"INFO", format, arguments);
    va_end(arguments);
}

void WarningLog(const wchar_t* format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    WriteLogLine(L"WARNING", format, arguments);
    va_end(arguments);
}

void ErrorLog(const wchar_t* format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    WriteLogLine(L"ERROR", format, arguments);
    va_end(arguments);
}

