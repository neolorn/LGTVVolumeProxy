#include "framework.h"
#include "LGTVVolumeProxy.h"

#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>
#include <wrl/client.h>
#include <shellapi.h>

#include <string>
#include <atomic>
#include <fstream>
#include <cwctype>
#include <cstdarg>
#include <cstdlib>

#include "AudioFormatAliases.h"
#include "Configuration.h"
#include "Logging.h"
#include "TVClient.h"

#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Mmdevapi.lib")
#pragma comment(lib, "Uuid.lib")
#pragma comment(lib, "Winhttp.lib")
#pragma comment(lib, "Shell32.lib")

#define MAX_LOADSTRING 100

using Microsoft::WRL::ComPtr;

HINSTANCE hInst;                                // current instance
WCHAR szTitle[MAX_LOADSTRING];                  // title bar text
WCHAR szWindowClass[MAX_LOADSTRING];            // main window class name

ATOM                MyRegisterClass(HINSTANCE hInstance);
BOOL                InitInstance(HINSTANCE, int);
LRESULT CALLBACK    WndProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK    About(HWND, UINT, WPARAM, LPARAM);

// UI control IDs for runtime-created controls.
#define IDC_EDIT_TVIP          2001
#define IDC_EDIT_TVMAC         2002
#define IDC_EDIT_DEVHINT       2003
#define IDC_CHECK_ONLYATMOS    2004
#define IDC_CHECK_USE_SECURE   2005
#define IDC_EDIT_TVPORT        2006
#define IDC_BUTTON_APPLY       2007
#define IDC_STATIC_STATUS      2008
#define IDC_BUTTON_PAIR        2009
#define IDC_BUTTON_UNPAIR      2010

// Tray icon callback and command identifiers.
static constexpr UINT WM_TRAYICON = WM_APP + 1;
#define IDM_TRAY_OPEN          41001
#define IDM_TRAY_EXIT          41002

/// Returns a lowercase copy of the given string using the current locale.
static std::wstring ToLower(const std::wstring& value)
{
    std::wstring result(value);
    for (wchar_t& character : result)
    {
        character = static_cast<wchar_t>(towlower(character));
    }
    return result;
}

// Forward declarations.
static void UpdateRouting();

// Global configuration shared across modules.
static AppConfiguration g_configuration;

// Audio and routing state.
static std::atomic<bool> g_defaultDeviceIsLg(false);
static std::atomic<bool> g_dolbyAtmosActive(false);
static std::atomic<bool> g_useTvVolume(false);
static std::atomic<bool> g_tvMuted(false);
static std::wstring g_defaultDeviceName;

static ComPtr<IAudioEndpointVolume> g_endpointVolume;
static float g_prevVolumeScalar = 0.25f;
static GUID  g_volumeEventContext = GUID_NULL;

// Global keyboard hook used to intercept volume keys.
static HHOOK g_hKeyboardHook = nullptr;

// Controls whether the app starts with the window hidden when paired.
static bool g_startMinimized = false;

// Allows WM_CLOSE to destroy the window when true.
static bool g_allowClose = false;

/// Holds handles to all runtime-created UI controls.
struct UiHandles
{
    HWND editTvIp{};
    HWND editTvMac{};
    HWND editDeviceHint{};
    HWND checkOnlyAtmos{};
    HWND checkUseSecure{};
    HWND editTvPort{};

    HWND statusConnectionValue{};
    HWND statusMacValue{};
    HWND statusDeviceNameValue{};
    HWND statusRoutingValue{};
    HWND statusDefaultLgValue{};
    HWND statusAtmosValue{};
    HWND statusPairingValue{};
};

/// Stores UI resources and provides helper operations for the main window.
namespace Ui
{
    static UiHandles g_handles;
    static HFONT g_font = nullptr;
    static NOTIFYICONDATAW g_trayIconData{};
    static bool g_trayIconCreated = false;

    /// Applies the shared UI font to the specified control.
    static void ApplyFont(HWND control)
    {
        if (g_font != nullptr && control != nullptr)
        {
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
        }
    }

    /// Updates the status text controls from the current configuration and state.
    static void UpdateStatusText()
    {
        if (!g_handles.statusConnectionValue)
        {
            return;
        }

        wchar_t buffer[128];

        swprintf_s(
            buffer,
            L"%s:%hu",
            g_configuration.tvIpAddress.c_str(),
            g_configuration.tvPort);
        SetWindowTextW(g_handles.statusConnectionValue, buffer);

        SetWindowTextW(g_handles.statusMacValue, g_configuration.tvMacAddress.c_str());

        const wchar_t* deviceNameToShow =
            !g_defaultDeviceName.empty()
            ? g_defaultDeviceName.c_str()
            : g_configuration.deviceNameHint.c_str();
        SetWindowTextW(g_handles.statusDeviceNameValue, deviceNameToShow);

        SetWindowTextW(
            g_handles.statusRoutingValue,
            g_useTvVolume.load() ? L"TV" : L"Windows");

        SetWindowTextW(
            g_handles.statusDefaultLgValue,
            g_defaultDeviceIsLg.load() ? L"Yes" : L"No");

        SetWindowTextW(
            g_handles.statusAtmosValue,
            g_dolbyAtmosActive.load() ? L"Yes" : L"No");

        SetWindowTextW(
            g_handles.statusPairingValue,
            GetTVClient().HasClientKey() ? L"Yes" : L"No");
    }

    /// Creates the tray icon associated with the main window.
    static void CreateTrayIcon(HWND windowHandle)
    {
        if (g_trayIconCreated)
        {
            return;
        }

        ZeroMemory(&g_trayIconData, sizeof(g_trayIconData));
        g_trayIconData.cbSize = sizeof(NOTIFYICONDATAW);
        g_trayIconData.hWnd = windowHandle;
        g_trayIconData.uID = 1;
        g_trayIconData.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        g_trayIconData.uCallbackMessage = WM_TRAYICON;
        g_trayIconData.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_LGTVVOLUMEPROXY));
        wcscpy_s(g_trayIconData.szTip, L"LG TV Volume Proxy");

        if (Shell_NotifyIconW(NIM_ADD, &g_trayIconData))
        {
            g_trayIconCreated = true;
        }
    }

    /// Removes the tray icon.
    static void DestroyTrayIcon()
    {
        if (!g_trayIconCreated)
        {
            return;
        }

        Shell_NotifyIconW(NIM_DELETE, &g_trayIconData);
        g_trayIconCreated = false;
    }

    /// Displays the tray icon context menu.
    static void ShowTrayMenu(HWND windowHandle)
    {
        POINT cursorPos{};
        if (!GetCursorPos(&cursorPos))
        {
            return;
        }

        HMENU menu = CreatePopupMenu();
        if (!menu)
        {
            return;
        }

        AppendMenuW(menu, MF_STRING, IDM_TRAY_OPEN, L"Open");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, IDM_TRAY_EXIT, L"Exit");

        SetForegroundWindow(windowHandle);
        TrackPopupMenu(
            menu,
            TPM_RIGHTBUTTON,
            cursorPos.x,
            cursorPos.y,
            0,
            windowHandle,
            nullptr);

        DestroyMenu(menu);
    }

    /// Handles mouse interaction with the tray icon.
    static void HandleTrayIconMessage(HWND windowHandle, LPARAM lParam)
    {
        switch (LOWORD(lParam))
        {
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
            ShowWindow(windowHandle, SW_SHOWNORMAL);
            ShowWindow(windowHandle, SW_RESTORE);
            SetForegroundWindow(windowHandle);
            break;

        case WM_RBUTTONUP:
            ShowTrayMenu(windowHandle);
            break;

        default:
            break;
        }
    }

    /// Shows the one-time hint explaining that close minimizes to tray.
    static void ShowCloseToTrayHint(HWND parentWindow)
    {
        if (!g_configuration.showCloseToTrayMessage)
        {
            return;
        }

        const wchar_t* message =
            L"Closing the window will minimize the app to the system tray instead of exiting.\n\n"
            L"Use Exit from the menu or the tray icon to quit the app.\n\n"
            L"Do you want to see this reminder again?";

        int result = MessageBoxW(
            parentWindow,
            message,
            L"LG TV Volume Proxy",
            MB_ICONINFORMATION | MB_YESNO | MB_DEFBUTTON1);

        if (result == IDNO)
        {
            g_configuration.showCloseToTrayMessage = false;
            SaveConfiguration(g_configuration);
        }
    }
}

// Watches audio endpoint changes and keeps routing state in sync.
class AudioEndpointWatcher : public IMMNotificationClient
{
public:
    AudioEndpointWatcher() : _refCount(1) {}

    HRESULT Initialize()
    {
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator),
            nullptr,
            CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator),
            (void**)_enumerator.GetAddressOf());
        if (FAILED(hr))
        {
            DebugLog(L"[Audio] MMDeviceEnumerator failed: 0x%08X\n", hr);
            return hr;
        }

        hr = _enumerator->RegisterEndpointNotificationCallback(this);
        if (FAILED(hr))
        {
            DebugLog(L"[Audio] RegisterEndpointNotificationCallback failed: 0x%08X\n", hr);
            return hr;
        }

        RefreshDefaultDevice();
        return S_OK;
    }

    void Shutdown()
    {
        if (_enumerator)
        {
            _enumerator->UnregisterEndpointNotificationCallback(this);
            _enumerator.Reset();
        }
    }

    // IUnknown
    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return InterlockedIncrement(&_refCount);
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG ulRef = InterlockedDecrement(&_refCount);
        if (ulRef == 0)
            delete this;
        return ulRef;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, VOID** ppvInterface) override
    {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient))
        {
            AddRef();
            *ppvInterface = (IMMNotificationClient*)this;
            return S_OK;
        }
        *ppvInterface = nullptr;
        return E_NOINTERFACE;
    }

    // IMMNotificationClient
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow,
        ERole role,
        LPCWSTR) override
    {
        if (flow == eRender && (role == eConsole || role == eMultimedia))
        {
            DebugLog(L"[Audio] OnDefaultDeviceChanged\n");
            RefreshDefaultDevice();
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
    HRESULT RefreshDefaultDevice()
    {
        ComPtr<IMMDevice> device;
        HRESULT hr = _enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        if (FAILED(hr))
        {
            DebugLog(L"[Audio] GetDefaultAudioEndpoint failed: 0x%08X\n", hr);
            g_defaultDeviceIsLg = false;
            g_dolbyAtmosActive = false;
            g_defaultDeviceName.clear();
            g_endpointVolume.Reset();
            UpdateRouting();
            return hr;
        }

        bool isLg = IsLgDevice(device.Get());
        g_defaultDeviceIsLg = isLg;

        bool atmos = false;
        if (isLg)
            atmos = IsDolbyAtmosAvailable(device.Get());
        g_dolbyAtmosActive = atmos;

        // Update endpoint volume interface
        g_endpointVolume.Reset();
        hr = device->Activate(__uuidof(IAudioEndpointVolume),
            CLSCTX_ALL,
            nullptr,
            (void**)g_endpointVolume.GetAddressOf());
        if (FAILED(hr))
        {
            DebugLog(L"[Audio] Activate(IAudioEndpointVolume) failed: 0x%08X\n", hr);
        }

        DebugLog(L"[Audio] Endpoint match: isLg=%d, atmos=%d\n", isLg ? 1 : 0, atmos ? 1 : 0);

        UpdateRouting();
        return S_OK;
    }

    bool IsLgDevice(IMMDevice* device)
    {
        if (!device)
        {
            g_defaultDeviceName.clear();
            return false;
        }

        ComPtr<IPropertyStore> props;
        HRESULT hr = device->OpenPropertyStore(STGM_READ, &props);
        if (FAILED(hr))
        {
            g_defaultDeviceName.clear();
            return false;
        }

        PROPVARIANT varName;
        PropVariantInit(&varName);

        hr = props->GetValue(PKEY_Device_FriendlyName, &varName);
        if (FAILED(hr))
        {
            PropVariantClear(&varName);
            g_defaultDeviceName.clear();
            return false;
        }

        bool result = false;
        if (varName.vt == VT_LPWSTR && varName.pwszVal)
        {
            std::wstring name(varName.pwszVal);
            g_defaultDeviceName = name;
            std::wstring nameLower = ToLower(name);
            std::wstring hintLower = ToLower(g_configuration.deviceNameHint);

            if (!hintLower.empty() &&
                nameLower.find(hintLower) != std::wstring::npos)
            {
                result = true;
            }

            DebugLog(L"[Audio] Endpoint name: %s, hint: %s, match=%d\n",
                name.c_str(), g_configuration.deviceNameHint.c_str(), result ? 1 : 0);
        }
        else
        {
            g_defaultDeviceName.clear();
        }

        PropVariantClear(&varName);
        return result;
    }

    bool IsDolbyAtmosAvailable(IMMDevice* device)
    {
        if (!device)
            return false;

        ComPtr<ISpatialAudioClient> spatial;
        HRESULT hr = device->Activate(__uuidof(ISpatialAudioClient),
            CLSCTX_INPROC_SERVER,
            nullptr,
            (void**)spatial.GetAddressOf());
        if (FAILED(hr) || !spatial)
        {
            DebugLog(L"[Audio] Activate(ISpatialAudioClient) failed: 0x%08X\n", hr);
            return false;
        }

        hr = spatial->IsSpatialAudioStreamAvailable(LGTV_SPATIAL_AUDIO_FORMAT_DOLBY_ATMOS,
            nullptr);
        if (hr == S_OK)
            return true;

        DebugLog(L"[Audio] IsSpatialAudioStreamAvailable returned: 0x%08X\n", hr);
        return false;
    }

    ComPtr<IMMDeviceEnumerator> _enumerator;
    LONG _refCount;
};

static AudioEndpointWatcher* g_endpointWatcher = nullptr;
/// Recomputes routing state and pins or restores the Windows endpoint volume.
static void UpdateRouting()
{
    bool useTv = false;

    if (g_defaultDeviceIsLg.load())
    {
        if (g_configuration.onlyWhenDolbyAtmos)
            useTv = g_dolbyAtmosActive.load();
        else
            useTv = true;
    }

    // TV routing is only active when the default device matches
    // and the app is currently paired with a TV.
    if (useTv && !GetTVClient().HasClientKey())
    {
        useTv = false;
    }

    bool prevUse = g_useTvVolume.load();
    g_useTvVolume.store(useTv);

    // Pin/unpin endpoint volume around routing toggle
    if (g_endpointVolume)
    {
        if (useTv && !prevUse)
        {
            float current = g_prevVolumeScalar;
            if (SUCCEEDED(g_endpointVolume->GetMasterVolumeLevelScalar(&current)))
            {
                g_prevVolumeScalar = current;
            }

            if (IsEqualGUID(g_volumeEventContext, GUID_NULL))
            {
                HRESULT guidResult = CoCreateGuid(&g_volumeEventContext);
                if (FAILED(guidResult))
                {
                    ErrorLog(L"[Audio] CoCreateGuid failed: 0x%08X", guidResult);
                }
            }

            g_endpointVolume->SetMasterVolumeLevelScalar(1.0f, &g_volumeEventContext);
        }
        else if (!useTv && prevUse)
        {
            if (IsEqualGUID(g_volumeEventContext, GUID_NULL))
            {
                HRESULT guidResult = CoCreateGuid(&g_volumeEventContext);
                if (FAILED(guidResult))
                {
                    ErrorLog(L"[Audio] CoCreateGuid failed: 0x%08X", guidResult);
                }
            }

            g_endpointVolume->SetMasterVolumeLevelScalar(g_prevVolumeScalar,
                &g_volumeEventContext);
        }
    }

    Ui::UpdateStatusText();
}

/// Creates all child controls in the main window based on the current configuration.
static void CreateChildControls(HWND hWnd)
{
    RECT clientRect{};
    GetClientRect(hWnd, &clientRect);
    const int clientWidth = clientRect.right - clientRect.left;

    const int marginX = 12;
    int y = 12;
    const int groupSpacing = 10;
    const int labelWidth = 140;
    const int controlHeight = 22;

    if (!Ui::g_font)
    {
        NONCLIENTMETRICSW metrics{};
        metrics.cbSize = sizeof(metrics);
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS,
            sizeof(metrics),
            &metrics,
            0))
        {
            Ui::g_font = CreateFontIndirectW(&metrics.lfMessageFont);
        }
        else
        {
            Ui::g_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        }
    }

    const int groupWidth = clientWidth - (marginX * 2);
    const int valueColumnX = marginX + 10 + labelWidth;
    const int fieldWidth = groupWidth - (labelWidth + 30);

    auto applyFont = [](HWND control)
    {
        Ui::ApplyFont(control);
    };

    // TV connection group
    HWND titleConnection = CreateWindowExW(
        0,
        L"STATIC",
        L"Connection",
        WS_CHILD | WS_VISIBLE,
        marginX,
        y,
        groupWidth,
        controlHeight,
        hWnd,
        nullptr,
        hInst,
        nullptr);
    applyFont(titleConnection);
    y += controlHeight;

    HWND groupConnection = CreateWindowExW(
        0,
        L"BUTTON",
        L"",
        WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
        marginX,
        y,
        groupWidth,
        150,
        hWnd,
        nullptr,
        hInst,
        nullptr);
    applyFont(groupConnection);

    int rowY = y + 18;

    HWND labelTvIp = CreateWindowExW(
        0,
        L"STATIC",
        L"IP Address:",
        WS_CHILD | WS_VISIBLE,
        marginX + 10,
        rowY,
        labelWidth,
        controlHeight,
        hWnd,
        nullptr,
        hInst,
        nullptr);
    applyFont(labelTvIp);

    Ui::g_handles.editTvIp = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        L"EDIT",
        g_configuration.tvIpAddress.c_str(),
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        valueColumnX,
        rowY - 1,
        fieldWidth,
        controlHeight,
        hWnd,
        (HMENU)IDC_EDIT_TVIP,
        hInst,
        nullptr);
    applyFont(Ui::g_handles.editTvIp);
    rowY += controlHeight + 6;

    HWND labelTvMac = CreateWindowExW(
        0,
        L"STATIC",
        L"MAC Address:",
        WS_CHILD | WS_VISIBLE,
        marginX + 10,
        rowY,
        labelWidth,
        controlHeight,
        hWnd,
        nullptr,
        hInst,
        nullptr);
    applyFont(labelTvMac);

    Ui::g_handles.editTvMac = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        L"EDIT",
        g_configuration.tvMacAddress.c_str(),
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        valueColumnX,
        rowY - 1,
        fieldWidth,
        controlHeight,
        hWnd,
        (HMENU)IDC_EDIT_TVMAC,
        hInst,
        nullptr);
    applyFont(Ui::g_handles.editTvMac);
    rowY += controlHeight + 6;

    HWND labelDeviceHint = CreateWindowExW(
        0,
        L"STATIC",
        L"Device Name Hint:",
        WS_CHILD | WS_VISIBLE,
        marginX + 10,
        rowY,
        labelWidth,
        controlHeight,
        hWnd,
        nullptr,
        hInst,
        nullptr);
    applyFont(labelDeviceHint);

    Ui::g_handles.editDeviceHint = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        L"EDIT",
        g_configuration.deviceNameHint.c_str(),
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        valueColumnX,
        rowY - 1,
        fieldWidth,
        controlHeight,
        hWnd,
        (HMENU)IDC_EDIT_DEVHINT,
        hInst,
        nullptr);
    applyFont(Ui::g_handles.editDeviceHint);
    rowY += controlHeight + 6;

    HWND labelTvPort = CreateWindowExW(
        0,
        L"STATIC",
        L"Port:",
        WS_CHILD | WS_VISIBLE,
        marginX + 10,
        rowY,
        labelWidth,
        controlHeight,
        hWnd,
        nullptr,
        hInst,
        nullptr);
    applyFont(labelTvPort);

    wchar_t portBuffer[16];
    swprintf_s(portBuffer, L"%hu", g_configuration.tvPort);
    Ui::g_handles.editTvPort = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        L"EDIT",
        portBuffer,
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        valueColumnX,
        rowY - 1,
        80,
        controlHeight,
        hWnd,
        (HMENU)IDC_EDIT_TVPORT,
        hInst,
        nullptr);
    applyFont(Ui::g_handles.editTvPort);
    rowY += controlHeight + 10;

    Ui::g_handles.checkUseSecure = CreateWindowExW(
        0,
        L"BUTTON",
        L"Use Secure WebSocket (WSS, port 3001)",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        marginX + 10,
        rowY,
        groupWidth - 20,
        controlHeight,
        hWnd,
        (HMENU)IDC_CHECK_USE_SECURE,
        hInst,
        nullptr);
    applyFont(Ui::g_handles.checkUseSecure);
    SendMessageW(
        Ui::g_handles.checkUseSecure,
        BM_SETCHECK,
        g_configuration.useSecureWebSocket ? BST_CHECKED : BST_UNCHECKED,
        0);
    rowY += controlHeight + 6;

    int connectionBottom = rowY;
    SetWindowPos(
        groupConnection,
        nullptr,
        marginX,
        y,
        groupWidth,
        connectionBottom - y,
        SWP_NOZORDER);

    y = connectionBottom + groupSpacing;

    // Routing group
    HWND titleRouting = CreateWindowExW(
        0,
        L"STATIC",
        L"Routing",
        WS_CHILD | WS_VISIBLE,
        marginX,
        y,
        groupWidth,
        controlHeight,
        hWnd,
        nullptr,
        hInst,
        nullptr);
    applyFont(titleRouting);
    y += controlHeight;

    HWND groupRouting = CreateWindowExW(
        0,
        L"BUTTON",
        L"",
        WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
        marginX,
        y,
        groupWidth,
        controlHeight * 2,
        hWnd,
        nullptr,
        hInst,
        nullptr);
    applyFont(groupRouting);

    int routingRowY = y + 18;
    Ui::g_handles.checkOnlyAtmos = CreateWindowExW(
        0,
        L"BUTTON",
        L"Use TV volume only when Dolby Atmos is active",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        marginX + 10,
        routingRowY,
        groupWidth - 20,
        controlHeight,
        hWnd,
        (HMENU)IDC_CHECK_ONLYATMOS,
        hInst,
        nullptr);
    applyFont(Ui::g_handles.checkOnlyAtmos);
    SendMessageW(
        Ui::g_handles.checkOnlyAtmos,
        BM_SETCHECK,
        g_configuration.onlyWhenDolbyAtmos ? BST_CHECKED : BST_UNCHECKED,
        0);

    int routingBottom = routingRowY + controlHeight + 10;
    SetWindowPos(
        groupRouting,
        nullptr,
        marginX,
        y,
        groupWidth,
        routingBottom - y,
        SWP_NOZORDER);

    y = routingBottom + groupSpacing;

    // Pairing and control group
    HWND titleControl = CreateWindowExW(
        0,
        L"STATIC",
        L"Control and Pairing",
        WS_CHILD | WS_VISIBLE,
        marginX,
        y,
        groupWidth,
        controlHeight,
        hWnd,
        nullptr,
        hInst,
        nullptr);
    applyFont(titleControl);
    y += controlHeight;

    HWND groupControl = CreateWindowExW(
        0,
        L"BUTTON",
        L"",
        WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
        marginX,
        y,
        groupWidth,
        controlHeight * 3,
        hWnd,
        nullptr,
        hInst,
        nullptr);
    applyFont(groupControl);

    int controlRowY = y + 22;
    const int buttonWidth = 100;
    const int buttonSpacing = 12;

    HWND buttonApply = CreateWindowExW(
        0,
        L"BUTTON",
        L"Apply",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        marginX + 14,
        controlRowY,
        buttonWidth,
        controlHeight + 4,
        hWnd,
        (HMENU)IDC_BUTTON_APPLY,
        hInst,
        nullptr);
    applyFont(buttonApply);

    HWND buttonPair = CreateWindowExW(
        0,
        L"BUTTON",
        L"Pair with TV",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        marginX + 14 + buttonWidth + buttonSpacing,
        controlRowY,
        buttonWidth + 20,
        controlHeight + 4,
        hWnd,
        (HMENU)IDC_BUTTON_PAIR,
        hInst,
        nullptr);
    applyFont(buttonPair);

    HWND buttonUnpair = CreateWindowExW(
        0,
        L"BUTTON",
        L"Unpair",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        marginX + 14 + (buttonWidth + buttonSpacing) * 2 + 20,
        controlRowY,
        buttonWidth,
        controlHeight + 4,
        hWnd,
        (HMENU)IDC_BUTTON_UNPAIR,
        hInst,
        nullptr);
    applyFont(buttonUnpair);

    int controlBottom = controlRowY + controlHeight + 14;
    SetWindowPos(
        groupControl,
        nullptr,
        marginX,
        y,
        groupWidth,
        controlBottom - y,
        SWP_NOZORDER);

    y = controlBottom + groupSpacing;

    // Status group
    HWND titleStatus = CreateWindowExW(
        0,
        L"STATIC",
        L"Status",
        WS_CHILD | WS_VISIBLE,
        marginX,
        y,
        groupWidth,
        controlHeight,
        hWnd,
        nullptr,
        hInst,
        nullptr);
    applyFont(titleStatus);
    y += controlHeight;

    HWND groupStatus = CreateWindowExW(
        0,
        L"BUTTON",
        L"",
        WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
        marginX,
        y,
        groupWidth,
        controlHeight * 8,
        hWnd,
        nullptr,
        hInst,
        nullptr);
    applyFont(groupStatus);

    int statusRowY = y + 18;
    const int statusLabelWidth = 100;
    const int statusValueX = marginX + 10 + statusLabelWidth + 6;
    const int statusValueWidth = groupWidth - (statusLabelWidth + 26);

    auto createStatusRow = [&](LPCWSTR labelText, HWND& valueHandle)
    {
        HWND label = CreateWindowExW(
            0,
            L"STATIC",
            labelText,
            WS_CHILD | WS_VISIBLE,
            marginX + 10,
            statusRowY,
            statusLabelWidth,
            controlHeight,
            hWnd,
            nullptr,
            hInst,
            nullptr);
        applyFont(label);

        valueHandle = CreateWindowExW(
            0,
            L"STATIC",
            L"",
            WS_CHILD | WS_VISIBLE,
            statusValueX,
            statusRowY,
            statusValueWidth,
            controlHeight,
            hWnd,
            nullptr,
            hInst,
            nullptr);
        applyFont(valueHandle);

        statusRowY += controlHeight + 4;
    };

    createStatusRow(L"Paired:", Ui::g_handles.statusPairingValue);
    createStatusRow(L"Connection:", Ui::g_handles.statusConnectionValue);
    createStatusRow(L"MAC Address:", Ui::g_handles.statusMacValue);
    createStatusRow(L"Device Name:", Ui::g_handles.statusDeviceNameValue);
    createStatusRow(L"Is Default Device:", Ui::g_handles.statusDefaultLgValue);
    createStatusRow(L"Dolby Atmos:", Ui::g_handles.statusAtmosValue);
    createStatusRow(L"Volume Routing:", Ui::g_handles.statusRoutingValue);

    int statusBottom = statusRowY + 8;
    SetWindowPos(
        groupStatus,
        nullptr,
        marginX,
        y,
        groupWidth,
        statusBottom - y,
        SWP_NOZORDER);

    Ui::UpdateStatusText();

    // Adjust window size to fit content and apply position.
    int desiredClientHeight = statusBottom + marginX;
    RECT windowRect{ 0, 0, clientWidth, desiredClientHeight };
    DWORD windowStyle = static_cast<DWORD>(GetWindowLongPtrW(hWnd, GWL_STYLE));
    DWORD windowExStyle = static_cast<DWORD>(GetWindowLongPtrW(hWnd, GWL_EXSTYLE));
    AdjustWindowRectEx(&windowRect, windowStyle, TRUE, windowExStyle);
    int windowWidth = windowRect.right - windowRect.left;
    int windowHeight = windowRect.bottom - windowRect.top;

    int targetX = 0;
    int targetY = 0;
    if (g_configuration.hasWindowPosition)
    {
        targetX = g_configuration.windowLeft;
        targetY = g_configuration.windowTop;
    }
    else
    {
        RECT workArea{};
        if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0))
        {
            int workWidth = workArea.right - workArea.left;
            int workHeight = workArea.bottom - workArea.top;
            targetX = workArea.left + (workWidth - windowWidth) / 2;
            targetY = workArea.top + (workHeight - windowHeight) / 2;
        }
        else
        {
            targetX = CW_USEDEFAULT;
            targetY = CW_USEDEFAULT;
        }
    }

    SetWindowPos(
        hWnd,
        nullptr,
        targetX,
        targetY,
        windowWidth,
        windowHeight,
        SWP_NOZORDER);
}

/// Reads values from the UI controls and saves the updated configuration.
static void ApplyConfigFromUI()
{
    wchar_t buf[256];

    if (Ui::g_handles.editTvIp)
    {
        GetWindowTextW(Ui::g_handles.editTvIp, buf, ARRAYSIZE(buf));
        g_configuration.tvIpAddress = buf;
    }
    if (Ui::g_handles.editTvMac)
    {
        GetWindowTextW(Ui::g_handles.editTvMac, buf, ARRAYSIZE(buf));
        g_configuration.tvMacAddress = buf;
    }
    if (Ui::g_handles.editDeviceHint)
    {
        GetWindowTextW(Ui::g_handles.editDeviceHint, buf, ARRAYSIZE(buf));
        g_configuration.deviceNameHint = buf;
    }
    if (Ui::g_handles.editTvPort)
    {
        GetWindowTextW(Ui::g_handles.editTvPort, buf, ARRAYSIZE(buf));
        int port = _wtoi(buf);
        if (port <= 0 || port > 65535)
        {
            port = g_configuration.useSecureWebSocket ? 3001 : 3000;
        }
        g_configuration.tvPort = static_cast<unsigned short>(port);
    }
    if (Ui::g_handles.checkOnlyAtmos)
    {
        g_configuration.onlyWhenDolbyAtmos =
            (SendMessageW(Ui::g_handles.checkOnlyAtmos, BM_GETCHECK, 0, 0) == BST_CHECKED);
    }
    if (Ui::g_handles.checkUseSecure)
    {
        g_configuration.useSecureWebSocket =
            (SendMessageW(Ui::g_handles.checkUseSecure, BM_GETCHECK, 0, 0) == BST_CHECKED);
    }

    SaveConfiguration(g_configuration);

    // Recalculate routing with new "onlyWhenAtmos" flag etc.
    UpdateRouting();
}

/// Represents a queued TV volume action to be processed off the hook/UI thread.
enum class TvVolumeAction
{
    VolumeUp = 0,
    VolumeDown = 1,
    ToggleMute = 2
};

/// Worker function that executes a TV volume action on a thread pool thread.
static DWORD WINAPI TvVolumeWorkerProc(_In_ LPVOID parameter)
{
    TvVolumeAction action =
        static_cast<TvVolumeAction>(reinterpret_cast<intptr_t>(parameter));

    bool handled = false;

    switch (action)
    {
    case TvVolumeAction::VolumeUp:
        handled = GetTVClient().VolumeUp();
        break;
    case TvVolumeAction::VolumeDown:
        handled = GetTVClient().VolumeDown();
        break;
    case TvVolumeAction::ToggleMute:
    {
        bool previousMuted = g_tvMuted.load();
        bool newMuted = !previousMuted;
        handled = GetTVClient().SetMute(newMuted);
        if (handled)
        {
            g_tvMuted.store(newMuted);
        }
        break;
    }
    default:
        break;
    }

    if (!handled)
    {
        DebugLog(L"[Key] TV volume command failed for action=%d",
            static_cast<int>(action));
    }

    return 0;
}

/// Low-level keyboard hook used to intercept global volume keys.
static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode >= 0)
    {
        KBDLLHOOKSTRUCT* pkb = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)
        {
            if (pkb->vkCode == VK_VOLUME_UP ||
                pkb->vkCode == VK_VOLUME_DOWN ||
                pkb->vkCode == VK_VOLUME_MUTE)
            {
                DebugLog(L"[Key] VK=%u, useTv=%d\n",
                    pkb->vkCode, g_useTvVolume.load() ? 1 : 0);

                if (g_useTvVolume.load())
                {
                    TvVolumeAction action = TvVolumeAction::VolumeUp;
                    switch (pkb->vkCode)
                    {
                    case VK_VOLUME_UP:
                        action = TvVolumeAction::VolumeUp;
                        break;
                    case VK_VOLUME_DOWN:
                        action = TvVolumeAction::VolumeDown;
                        break;
                    case VK_VOLUME_MUTE:
                        action = TvVolumeAction::ToggleMute;
                        break;
                    default:
                        break;
                    }

                    if (!QueueUserWorkItem(
                        TvVolumeWorkerProc,
                        reinterpret_cast<LPVOID>(static_cast<intptr_t>(action)),
                        WT_EXECUTEDEFAULT))
                    {
                        ErrorLog(L"[Key] QueueUserWorkItem failed: %lu", GetLastError());
                    }

                    // Keep endpoint volume pinned to 100% while routing to TV.
                    if (g_endpointVolume)
                    {
                        if (IsEqualGUID(g_volumeEventContext, GUID_NULL))
                        {
                            HRESULT guidResult = CoCreateGuid(&g_volumeEventContext);
                            if (FAILED(guidResult))
                            {
                                ErrorLog(L"[Audio] CoCreateGuid failed: 0x%08X", guidResult);
                            }
                        }
                        g_endpointVolume->SetMasterVolumeLevelScalar(
                            1.0f, &g_volumeEventContext);
                    }

                    // Swallow the key so Windows does not also change volume,
                    // even if the TV command later fails.
                    return 1;
                }
            }
        }
    }

    return CallNextHookEx(g_hKeyboardHook, nCode, wParam, lParam);
}

/// Application entry point.
int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPWSTR    lpCmdLine,
    _In_ int       nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr))
        return -1;

    // Load configuration from disk (if present)
    LoadConfiguration(g_configuration);
    InitializeTVClient(&g_configuration);
    g_startMinimized = GetTVClient().HasClientKey();

    LoadStringW(hInstance, IDS_APP_TITLE, szTitle, MAX_LOADSTRING);
    LoadStringW(hInstance, IDC_LGTVVOLUMEPROXY, szWindowClass, MAX_LOADSTRING);
    MyRegisterClass(hInstance);

    if (!InitInstance(hInstance, nCmdShow))
    {
        CoUninitialize();
        return FALSE;
    }

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    CoUninitialize();
    return (int)msg.wParam;
}

/// Registers the main window class.
ATOM MyRegisterClass(HINSTANCE hInstance)
{
    WNDCLASSEXW wcex{};

    wcex.cbSize = sizeof(WNDCLASSEX);

    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = WndProc;
    wcex.cbClsExtra = 0;
    wcex.cbWndExtra = 0;
    wcex.hInstance = hInstance;
    wcex.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_LGTVVOLUMEPROXY));
    wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wcex.lpszMenuName = MAKEINTRESOURCEW(IDC_LGTVVOLUMEPROXY);
    wcex.lpszClassName = szWindowClass;
    wcex.hIconSm = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_SMALL));

    return RegisterClassExW(&wcex);
}

/// Creates and shows the main application window.
BOOL InitInstance(HINSTANCE hInstance, int nCmdShow)
{
    hInst = hInstance;

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;

    HWND hWnd = CreateWindowW(szWindowClass, szTitle, style,
        CW_USEDEFAULT, 0, 720, 420,
        nullptr, nullptr, hInstance, nullptr);

    if (!hWnd)
        return FALSE;

    int showCommand = nCmdShow;
    if (g_startMinimized)
    {
        showCommand = SW_HIDE;
    }

    ShowWindow(hWnd, showCommand);
    UpdateWindow(hWnd);

    Ui::CreateTrayIcon(hWnd);

    // Create audio endpoint watcher
    g_endpointWatcher = new AudioEndpointWatcher();
    if (g_endpointWatcher)
        g_endpointWatcher->Initialize();

    // Install low-level keyboard hook
    g_hKeyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL,
        LowLevelKeyboardProc,
        GetModuleHandleW(nullptr),
        0);
    if (!g_hKeyboardHook)
    {
        DebugLog(L"[Key] SetWindowsHookEx failed: %lu\n", GetLastError());
    }

    return TRUE;
}

/// Window procedure for the main application window.
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_CREATE:
        CreateChildControls(hWnd);
        break;

    case WM_COMMAND:
    {
        int wmId = LOWORD(wParam);

        switch (wmId)
        {
        case IDM_ABOUT:
            DialogBox(hInst, MAKEINTRESOURCE(IDD_ABOUTBOX), hWnd, About);
            break;

        case IDM_EXIT:
            g_allowClose = true;
            DestroyWindow(hWnd);
            break;

        case IDM_TRAY_OPEN:
            ShowWindow(hWnd, SW_SHOWNORMAL);
            ShowWindow(hWnd, SW_RESTORE);
            SetForegroundWindow(hWnd);
            break;

        case IDM_TRAY_EXIT:
            g_allowClose = true;
            DestroyWindow(hWnd);
            break;

        case IDC_BUTTON_APPLY:
            DebugLog(L"[UI] Apply clicked\n");
            ApplyConfigFromUI();
            Ui::UpdateStatusText();
            break;

        case IDC_BUTTON_PAIR:
        {
            DebugLog(L"[UI] Pair button clicked\n");

            // Make sure configuration is up to date (IP/port might have changed)
            ApplyConfigFromUI();
            Ui::UpdateStatusText();

            if (GetTVClient().PairWithTv(hWnd))
            {
                DebugLog(L"[UI] PairWithTv() succeeded\n");

                if (g_endpointVolume)
                {
                    if (IsEqualGUID(g_volumeEventContext, GUID_NULL))
                    {
                        HRESULT guidResult = CoCreateGuid(&g_volumeEventContext);
                        if (FAILED(guidResult))
                        {
                            ErrorLog(L"[Audio] CoCreateGuid failed: 0x%08X", guidResult);
                        }
                    }
                    g_endpointVolume->SetMasterVolumeLevelScalar(1.0f, &g_volumeEventContext);
                }

                GetTVClient().SetVolume(10);

                MessageBoxW(
                    hWnd,
                    L"Successfully paired with TV.\n\nFuture volume commands will use the TV directly when routing is active.",
                    L"LG TV Volume Proxy",
                    MB_OK | MB_ICONINFORMATION);
            }
            else
            {
                DebugLog(L"[UI] PairWithTv() FAILED\n");
                MessageBoxW(
                    hWnd,
                    L"Pairing failed.\n\nCheck:\n- TV IP / port / ws vs wss\n- TV is on and on the same network\n- You accepted the prompt on the TV.",
                    L"LG TV Volume Proxy",
                    MB_OK | MB_ICONERROR);
            }

            UpdateRouting();
            Ui::UpdateStatusText();
            break;
        }

        case IDC_BUTTON_UNPAIR:
        {
            DebugLog(L"[UI] Unpair button clicked\n");

            if (!GetTVClient().HasClientKey())
            {
                MessageBoxW(
                    hWnd,
                    L"The app is not currently paired with a TV.",
                    L"LG TV Volume Proxy",
                    MB_OK | MB_ICONINFORMATION);
                break;
            }

            // Set both Windows and TV volume to a safe fallback
            // before removing pairing information.
            if (g_endpointVolume)
            {
                if (IsEqualGUID(g_volumeEventContext, GUID_NULL))
                {
                    HRESULT guidResult = CoCreateGuid(&g_volumeEventContext);
                    if (FAILED(guidResult))
                    {
                        ErrorLog(L"[Audio] CoCreateGuid failed: 0x%08X", guidResult);
                    }
                }
                g_endpointVolume->SetMasterVolumeLevelScalar(0.10f, &g_volumeEventContext);
            }

            GetTVClient().SetVolume(10);

            int result = MessageBoxW(
                hWnd,
                L"This will remove the stored pairing information for the TV.\n\n"
                L"Do you want to continue?",
                L"LG TV Volume Proxy",
                MB_YESNO | MB_ICONQUESTION);
            if (result == IDYES)
            {
                if (GetTVClient().UnpairFromTv())
                {
                    DebugLog(L"[UI] UnpairFromTv() succeeded\n");
                    MessageBoxW(
                        hWnd,
                        L"Pairing information removed.\n\nYou will need to pair again before using TV volume control.",
                        L"LG TV Volume Proxy",
                        MB_OK | MB_ICONINFORMATION);
                }
                else
                {
                    DebugLog(L"[UI] UnpairFromTv() FAILED\n");
                    MessageBoxW(
                        hWnd,
                        L"Failed to remove pairing information.",
                        L"LG TV Volume Proxy",
                        MB_OK | MB_ICONERROR);
                }

                UpdateRouting();
                Ui::UpdateStatusText();
            }
            break;
        }

        default:
            return DefWindowProc(hWnd, message, wParam, lParam);
        }
    }
    break;

    case WM_TRAYICON:
        Ui::HandleTrayIconMessage(hWnd, lParam);
        break;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        EndPaint(hWnd, &ps);
    }
    break;

    case WM_CTLCOLORSTATIC:
    {
        HDC hdcStatic = (HDC)wParam;
        SetBkMode(hdcStatic, OPAQUE);
        SetTextColor(hdcStatic, GetSysColor(COLOR_WINDOWTEXT));
        SetBkColor(hdcStatic, GetSysColor(COLOR_WINDOW));
        return (INT_PTR)GetSysColorBrush(COLOR_WINDOW);
    }
    break;

    case WM_CLOSE:
        if (!g_allowClose)
        {
            Ui::ShowCloseToTrayHint(hWnd);
            ShowWindow(hWnd, SW_HIDE);
            return 0;
        }
        return DefWindowProc(hWnd, message, wParam, lParam);

    case WM_DESTROY:
    {
        // Restore audio to a safe fallback before exiting.
        if (g_endpointVolume)
        {
            if (IsEqualGUID(g_volumeEventContext, GUID_NULL))
            {
                HRESULT guidResult = CoCreateGuid(&g_volumeEventContext);
                if (FAILED(guidResult))
                {
                    ErrorLog(L"[Audio] CoCreateGuid failed: 0x%08X", guidResult);
                }
            }
            g_endpointVolume->SetMasterVolumeLevelScalar(0.10f, &g_volumeEventContext);
        }

        if (GetTVClient().HasClientKey())
        {
            GetTVClient().SetVolume(10);
        }

        if (g_hKeyboardHook)
        {
            UnhookWindowsHookEx(g_hKeyboardHook);
            g_hKeyboardHook = nullptr;
        }
        if (g_endpointWatcher)
        {
            g_endpointWatcher->Shutdown();
            g_endpointWatcher->Release();
            g_endpointWatcher = nullptr;
        }

        RECT windowRect{};
        if (GetWindowRect(hWnd, &windowRect))
        {
            g_configuration.windowLeft = windowRect.left;
            g_configuration.windowTop = windowRect.top;
            g_configuration.hasWindowPosition = true;
            SaveConfiguration(g_configuration);
        }
        Ui::DestroyTrayIcon();

        if (Ui::g_font)
        {
            DeleteObject(Ui::g_font);
            Ui::g_font = nullptr;
        }
        PostQuitMessage(0);
        break;
    }

    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }

    return 0;
}

/// Dialog procedure for the About box.
INT_PTR CALLBACK About(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(lParam);
    switch (message)
    {
    case WM_INITDIALOG:
        return (INT_PTR)TRUE;

    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)
        {
            EndDialog(hDlg, LOWORD(wParam));
            return (INT_PTR)TRUE;
        }
        break;
    }
    return (INT_PTR)FALSE;
}
