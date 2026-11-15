// Skyline — Copyright © 2026 Yen-Zen & Cyber. All rights reserved. See LICENSE.txt.
// Native Flyouts — see flyouts.h
//
// Sound   : Core Audio (IAudioEndpointVolume through StatusMonitor, IAudioSessionManager2 per app).
//           Picking the output device uses IPolicyConfig: undocumented, but used by Windows itself and
//           by many well-known tools for years. If it's missing, the device list is just for show.
// Wi-Fi   : the WLAN API (wlanapi), the same one the status icon already listens to.
//           Newer Windows 11 asks for location access before apps may see networks: we say so.
// Radios  : Windows.Devices.Radios (WinRT, declared by hand like Now Playing) on a short-lived thread.
// Bluetooth devices: the classic Bluetooth API from bthprops.cpl, loaded only when the panel opens.
#include "flyouts.h"
#include "app.h"
#include "icons.h"
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <audiopolicy.h>
#include <wlanapi.h>
#include <bluetoothapis.h>
#include <roapi.h>
#include <winstring.h>
#include <inspectable.h>
#include <thread>
#include <map>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <shlobj.h>
#include <knownfolders.h>

namespace {

constexpr UINT WM_FLY_RADIO = WM_APP + 51;   // wp = Wi-Fi radio state, lp = Bluetooth radio state (-1 none)
constexpr UINT_PTR T_CONNECT = 1, T_CARET = 2;
constexpr float kW = 340, kPad = 14;

// ---------------------------------------------------------------------------
// Small helpers (local copies, so this file stands on its own)
// ---------------------------------------------------------------------------
D2D1_COLOR_F A(D2D1_COLOR_F c, float a) { c.a *= a; return c; }
bool In(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

const wchar_t* GlyphFont() {
    static std::wstring f;
    if (f.empty()) {
        f = L"Segoe MDL2 Assets";
        ComPtr<IDWriteFontCollection> fc;
        if (SUCCEEDED(g_gfx.dwrite->GetSystemFontCollection(fc.Put(), FALSE)) && fc) {
            UINT32 idx;
            BOOL ex = FALSE;
            if (SUCCEEDED(fc->FindFamilyName(L"Segoe Fluent Icons", &idx, &ex)) && ex) f = L"Segoe Fluent Icons";
        }
    }
    return f.c_str();
}

std::wstring Utf8(const unsigned char* p, size_t n) {
    int w = MultiByteToWideChar(CP_UTF8, 0, (const char*)p, (int)n, nullptr, 0);
    std::wstring s(w, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, (const char*)p, (int)n, s.data(), w);
    return s;
}

std::wstring XmlEscape(const std::wstring& s) {
    std::wstring o;
    for (wchar_t c : s) {
        switch (c) {
        case L'&': o += L"&amp;"; break;
        case L'<': o += L"&lt;"; break;
        case L'>': o += L"&gt;"; break;
        case L'"': o += L"&quot;"; break;
        case L'\'': o += L"&apos;"; break;
        default: o += c;
        }
    }
    return o;
}

// ---------------------------------------------------------------------------
// IPolicyConfig (set the default output device)
// ---------------------------------------------------------------------------
const GUID CLSID_PolicyConfig = {0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};
const GUID IID_PolicyConfig = {0xf8679f50, 0x850a, 0x41cf, {0x9c, 0x72, 0x43, 0x0f, 0x29, 0x02, 0x90, 0xc8}};
struct IPolicyConfigAbi : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, void*, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR, ERole) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};
const PROPERTYKEY kFriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

// ---------------------------------------------------------------------------
// Windows.Devices.Radios (WinRT ABI by hand)
// ---------------------------------------------------------------------------
const GUID IID_AsyncInfo2 = {0x00000036, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
const GUID IID_RadioStatics = {0x5fb6a12e, 0x67cb, 0x46ae, {0xaa, 0xe9, 0x65, 0x91, 0x9f, 0x86, 0xef, 0xf4}};
const GUID IID_Radio = {0x252118df, 0xb33e, 0x416a, {0x87, 0x5f, 0x1c, 0xf3, 0x8a, 0xe2, 0xd8, 0x3e}};
struct IAsyncInfoAbi2 : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Id(UINT32*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Status(INT32*) = 0;   // 0 started, 1 completed, 2 canceled, 3 error
    virtual HRESULT STDMETHODCALLTYPE get_ErrorCode(HRESULT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE Cancel() = 0;
    virtual HRESULT STDMETHODCALLTYPE Close() = 0;
};
struct IAsyncOpAbi2 : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE put_Completed(void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Completed(void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetResults(void*) = 0;
};
struct IRadioStaticsAbi : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE GetRadiosAsync(IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceSelector(HSTRING*) = 0;
    virtual HRESULT STDMETHODCALLTYPE FromIdAsync(HSTRING, IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE RequestAccessAsync(IInspectable**) = 0;
};
struct IRadioAbi : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE SetStateAsync(INT32, IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_StateChanged(void*, INT64*) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_StateChanged(INT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_State(INT32*) = 0;      // 0 unknown, 1 on, 2 off, 3 disabled
    virtual HRESULT STDMETHODCALLTYPE get_Name(HSTRING*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Kind(INT32*) = 0;       // 1 Wi-Fi, 3 Bluetooth
};
struct IVectorViewAbi : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE GetAt(UINT32, IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Size(UINT32*) = 0;
};

// Waits (on the worker thread) for a WinRT async operation. true = completed.
bool Wait(IInspectable* op) {
    ComPtr<IAsyncInfoAbi2> info;
    if (!op || FAILED(op->QueryInterface(IID_AsyncInfo2, reinterpret_cast<void**>(info.Put())))) return false;
    for (int i = 0; i < 400; ++i) {   // at most ~6 s
        INT32 st = 0;
        if (FAILED(info->get_Status(&st))) return false;
        if (st == 1) return true;
        if (st != 0) return false;
        Sleep(15);
    }
    return false;
}

// Reads both radios and, if asked, switches one of them first. Posts the states to `notify`.
void RadioJob(HWND notify, int setKind, int setState) {
    int wifi = -1, bt = -1;
    if (SUCCEEDED(RoInitialize(RO_INIT_MULTITHREADED))) {
        HSTRING cls = nullptr;
        const wchar_t* name = L"Windows.Devices.Radios.Radio";
        ComPtr<IRadioStaticsAbi> statics;
        if (SUCCEEDED(WindowsCreateString(name, (UINT32)wcslen(name), &cls)) &&
            SUCCEEDED(RoGetActivationFactory(cls, IID_RadioStatics, reinterpret_cast<void**>(statics.Put()))) && statics) {
            ComPtr<IInspectable> op;
            if (SUCCEEDED(statics->RequestAccessAsync(op.Put()))) Wait(op.Get());
            op.Reset();
            ComPtr<IVectorViewAbi> list;
            if (SUCCEEDED(statics->GetRadiosAsync(op.Put())) && Wait(op.Get()))
                static_cast<IAsyncOpAbi2*>(op.Get())->GetResults(list.Put());
            UINT32 n = 0;
            if (list && SUCCEEDED(list->get_Size(&n))) {
                for (UINT32 i = 0; i < n; ++i) {
                    ComPtr<IInspectable> item;
                    ComPtr<IRadioAbi> r;
                    if (FAILED(list->GetAt(i, item.Put())) || !item || FAILED(item->QueryInterface(IID_Radio, reinterpret_cast<void**>(r.Put()))))
                        continue;
                    INT32 kind = 0, state = 0;
                    r->get_Kind(&kind);
                    if (kind != 1 && kind != 3) continue;
                    if (kind == setKind) {
                        ComPtr<IInspectable> sop;
                        if (SUCCEEDED(r->SetStateAsync(setState, sop.Put()))) Wait(sop.Get());
                    }
                    r->get_State(&state);
                    if (kind == 1 && wifi != 1) wifi = state;   // several adapters: "on" wins
                    if (kind == 3 && bt != 1) bt = state;
                }
            }
        }
        if (cls) WindowsDeleteString(cls);
        RoUninitialize();
    }
    if (IsWindow(notify)) PostMessageW(notify, WM_FLY_RADIO, (WPARAM)(INT_PTR)wifi, (LPARAM)(INT_PTR)bt);
}

// ---------------------------------------------------------------------------
// Bluetooth devices (classic API from bthprops.cpl, loaded on first use)
// ---------------------------------------------------------------------------
struct BtDev { std::wstring name; bool connected = false; ULONG cod = 0; };

std::vector<BtDev> PairedBluetooth() {
    using FindFirst = HBLUETOOTH_DEVICE_FIND(WINAPI*)(const BLUETOOTH_DEVICE_SEARCH_PARAMS*, BLUETOOTH_DEVICE_INFO*);
    using FindNext = BOOL(WINAPI*)(HBLUETOOTH_DEVICE_FIND, BLUETOOTH_DEVICE_INFO*);
    using FindClose = BOOL(WINAPI*)(HBLUETOOTH_DEVICE_FIND);
    static HMODULE mod = LoadLibraryExW(L"bthprops.cpl", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    std::vector<BtDev> out;
    if (!mod) return out;
    auto first = (FindFirst)GetProcAddress(mod, "BluetoothFindFirstDevice");
    auto next = (FindNext)GetProcAddress(mod, "BluetoothFindNextDevice");
    auto close = (FindClose)GetProcAddress(mod, "BluetoothFindDeviceClose");
    if (!first || !next || !close) return out;
    BLUETOOTH_DEVICE_SEARCH_PARAMS sp{sizeof sp};
    sp.fReturnAuthenticated = TRUE;
    sp.fReturnRemembered = TRUE;
    sp.fReturnConnected = TRUE;
    sp.fReturnUnknown = FALSE;
    sp.fIssueInquiry = FALSE;   // what Windows already knows: instant, no radio search
    BLUETOOTH_DEVICE_INFO di{sizeof di};
    HBLUETOOTH_DEVICE_FIND f = first(&sp, &di);
    if (!f) return out;
    do {
        if (di.fRemembered || di.fAuthenticated || di.fConnected) {
            BtDev d;
            d.name = di.szName;
            d.connected = di.fConnected != FALSE;
            d.cod = di.ulClassofDevice;
            if (!d.name.empty()) out.push_back(d);
        }
        di = BLUETOOTH_DEVICE_INFO{sizeof di};
    } while (next(f, &di));
    close(f);
    std::stable_sort(out.begin(), out.end(), [](const BtDev& a, const BtDev& b) { return a.connected > b.connected; });
    if (out.size() > 6) out.resize(6);
    return out;
}

wchar_t BtGlyph(ULONG cod) {
    int major = (cod >> 8) & 0x1F;
    if (major == 4) return 0xE7F6;   // audio: headphones
    if (major == 2) return 0xE8EA;   // phone
    if (major == 1) return 0xE7F8;   // computer
    if (major == 5) {
        int minor = (cod >> 6) & 3;
        if (minor == 1) return 0xE765;   // keyboard
        if (minor == 2) return 0xE962;   // mouse
        return 0xE7FC;                   // other input: gamepad
    }
    return 0xE702;
}

// ---------------------------------------------------------------------------
// The panel
// ---------------------------------------------------------------------------
class Panel {
public:
    bool Visible() const { return hwnd_ && IsWindowVisible(hwnd_); }
    Flyouts::Kind Kind() const { return kind_; }
    DWORD HiddenAt() const { return hiddenAt_; }

    void Show(Flyouts::Kind kind, RECT anchor, UINT dpi) {
        kind_ = kind;
        anchor_ = anchor;
        if (dpi != dpi_) { dpi_ = dpi; DropResources(); }
        if (!Create()) return;
        BOOL dark = g_app.theme.dark;   // glass and theme may have changed since last time
        DwmSetWindowAttribute(hwnd_, 20, &dark, sizeof dark);
        ApplyGlassEffect(hwnd_, Part::Flyout);
        expanded_ = -1;
        pwActive_ = false;
        password_.clear();
        message_.clear();
        hover_ = drag_ = -1;
        if (kind_ == Flyouts::Sound) LoadSound();
        else if (kind_ == Flyouts::Network) LoadNetwork(true);
        else if (kind_ == Flyouts::Stack) LoadStack();
        else { SYSTEMTIME t; GetLocalTime(&t); calY_ = t.wYear; calM_ = t.wMonth; }   // the calendar opens on this month
        Place(true);
        SetForegroundWindow(hwnd_);
        if (GetForegroundWindow() != hwnd_) {   // opened from a hot corner (no click): Windows needs a nudge
            INPUT in{};
            in.type = INPUT_MOUSE;
            SendInput(1, &in, sizeof in);
            SetForegroundWindow(hwnd_);
        }
        SetFocus(hwnd_);
    }

    void Hide() {
        if (!hwnd_ || !IsWindowVisible(hwnd_)) return;
        KillTimer(hwnd_, T_CARET);
        if (ReleaseCapture()) {}
        ShowWindow(hwnd_, SW_HIDE);
        hiddenAt_ = GetTickCount();
        sessions_.clear();   // let go of the apps' audio sessions
        appBmps_.clear();
        stack_.clear();      // and the stack's file list and icons
    }

    void Refresh() {
        if (!Visible()) return;
        if (kind_ == Flyouts::Calendar || kind_ == Flyouts::Stack) return;
        if (kind_ == Flyouts::Sound) {
            if (drag_ < 0) LoadSound();
        } else {
            LoadNetwork(false);
            // waiting for a connection: done when Windows reports it
            if (!connecting_.empty()) {
                const StatusState& st = g_app.status->State();
                if (st.net == StatusState::Wifi && st.ssid == connecting_) { connecting_.clear(); message_.clear(); KillTimer(hwnd_, T_CONNECT); }
            }
        }
        Place(false);
    }

    void Destroy() {
        if (hwnd_) { SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0); DestroyWindow(hwnd_); hwnd_ = nullptr; }
        connecting_.clear();   // its timer went with the window: don't show "Connecting …" forever
        madeProfile_.clear();
        DropResources();
        radioBusy_ = false;   // a radio job still running posts to the old window: don't wait for it
    }

private:
    enum HitKind { HMute, HMaster, HDevice, HAppMute, HAppVol, HLink, HWifiToggle, HBtToggle, HNet, HNetAction, HPassword, HReveal, HBt, HLocation,
                   HCalPrev, HCalNext, HCalToday, HStackItem };
    struct Hit { D2D1_RECT_F r; HitKind k; int idx; };
    struct Device { std::wstring id, name; bool def = false; };
    struct Session {
        std::wstring key, path, name;
        bool system = false;
        std::vector<ComPtr<ISimpleAudioVolume>> vols;
        float vol = 1;
        bool muted = false;
    };
    struct Net {
        std::wstring ssid, profile;
        DOT11_SSID raw{};
        int quality = 0, auth = 0, cipher = 0;
        bool secured = false, hasProfile = false, connected = false;
    };

    // ---- data ----
    void LoadSound() {
        devices_.clear();
        ComPtr<IMMDeviceEnumerator> en;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)en.Put())) || !en) return;
        std::wstring defId;
        ComPtr<IMMDevice> def;
        if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eMultimedia, def.Put())) && def) {
            PWSTR id = nullptr;
            if (SUCCEEDED(def->GetId(&id)) && id) { defId = id; CoTaskMemFree(id); }
        }
        ComPtr<IMMDeviceCollection> col;
        UINT n = 0;
        if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, col.Put())) && col && SUCCEEDED(col->GetCount(&n))) {
            for (UINT i = 0; i < n; ++i) {
                ComPtr<IMMDevice> d;
                if (FAILED(col->Item(i, d.Put())) || !d) continue;
                Device dv;
                PWSTR id = nullptr;
                if (SUCCEEDED(d->GetId(&id)) && id) { dv.id = id; CoTaskMemFree(id); }
                ComPtr<IPropertyStore> ps;
                if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, ps.Put())) && ps) {
                    PROPVARIANT v;
                    PropVariantInit(&v);
                    if (SUCCEEDED(ps->GetValue(kFriendlyName, &v)) && v.vt == VT_LPWSTR && v.pwszVal) dv.name = v.pwszVal;
                    PropVariantClear(&v);
                }
                dv.def = dv.id == defId;
                if (!dv.name.empty()) devices_.push_back(dv);
            }
        }
        // The apps playing on the default device, one row per program
        std::vector<Session> found;
        ComPtr<IAudioSessionManager2> mgr;
        ComPtr<IAudioSessionEnumerator> se;
        int count = 0;
        if (def && SUCCEEDED(def->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, (void**)mgr.Put())) && mgr &&
            SUCCEEDED(mgr->GetSessionEnumerator(se.Put())) && se && SUCCEEDED(se->GetCount(&count))) {
            for (int i = 0; i < count; ++i) {
                ComPtr<IAudioSessionControl> c;
                ComPtr<IAudioSessionControl2> c2;
                ComPtr<ISimpleAudioVolume> sv;
                if (FAILED(se->GetSession(i, c.Put())) || !c) continue;
                AudioSessionState state;
                if (SUCCEEDED(c->GetState(&state)) && state == AudioSessionStateExpired) continue;
                if (FAILED(c->QueryInterface(__uuidof(IAudioSessionControl2), (void**)c2.Put())) || !c2) continue;
                if (FAILED(c->QueryInterface(__uuidof(ISimpleAudioVolume), (void**)sv.Put())) || !sv) continue;
                Session s;
                s.system = c2->IsSystemSoundsSession() == S_OK;
                DWORD pid = 0;
                c2->GetProcessId(&pid);
                if (s.system) { s.key = L"system"; s.name = TR(L"Systemlyder", L"System sounds"); }
                else {
                    s.path = ProcessImagePath(pid);
                    if (s.path.empty()) continue;
                    s.key = ToLower(s.path);
                    s.name = FileDescriptionOf(s.path);
                    if (s.name.empty()) s.name = FileNameOf(s.path);
                }
                auto it = std::find_if(found.begin(), found.end(), [&](const Session& x) { return x.key == s.key; });
                Session& dst = it != found.end() ? *it : (found.push_back(s), found.back());
                float v = 1;
                BOOL m = FALSE;
                sv->GetMasterVolume(&v);
                sv->GetMute(&m);
                if (dst.vols.empty()) { dst.vol = v; dst.muted = m != FALSE; }
                dst.vols.push_back(sv);
            }
        }
        // system sounds last, then by name
        std::stable_sort(found.begin(), found.end(), [](const Session& a, const Session& b) {
            if (a.system != b.system) return !a.system;
            return lstrcmpiW(a.name.c_str(), b.name.c_str()) < 0;
        });
        if (found.size() > 6) found.resize(6);
        sessions_ = std::move(found);
    }

    void LoadNetwork(bool scan) {
        nets_.clear();
        wifiDenied_ = false;
        hasWifi_ = false;
        if (!wlan_) {
            DWORD ver = 0;
            if (WlanOpenHandle(2, nullptr, &ver, &wlan_) != ERROR_SUCCESS) wlan_ = nullptr;
        }
        if (wlan_) {
            PWLAN_INTERFACE_INFO_LIST ifs = nullptr;
            if (WlanEnumInterfaces(wlan_, nullptr, &ifs) == ERROR_SUCCESS && ifs) {
                if (ifs->dwNumberOfItems > 0) {
                    hasWifi_ = true;
                    iface_ = ifs->InterfaceInfo[0].InterfaceGuid;
                    for (DWORD i = 0; i < ifs->dwNumberOfItems; ++i)   // prefer the one that's connected
                        if (ifs->InterfaceInfo[i].isState == wlan_interface_state_connected) { iface_ = ifs->InterfaceInfo[i].InterfaceGuid; break; }
                }
                WlanFreeMemory(ifs);
            }
            if (hasWifi_) {
                if (scan) WlanScan(wlan_, &iface_, nullptr, nullptr, nullptr);   // results arrive as a notification
                PWLAN_AVAILABLE_NETWORK_LIST list = nullptr;
                DWORD r = WlanGetAvailableNetworkList(wlan_, &iface_, 0, nullptr, &list);
                if (r == ERROR_ACCESS_DENIED) wifiDenied_ = true;
                if (r == ERROR_SUCCESS && list) {
                    for (DWORD i = 0; i < list->dwNumberOfItems; ++i) {
                        const WLAN_AVAILABLE_NETWORK& a = list->Network[i];
                        if (a.dot11Ssid.uSSIDLength == 0) continue;   // hidden network
                        Net n;
                        n.raw = a.dot11Ssid;
                        n.ssid = Utf8(a.dot11Ssid.ucSSID, a.dot11Ssid.uSSIDLength);
                        n.quality = (int)a.wlanSignalQuality;
                        n.secured = a.bSecurityEnabled != FALSE;
                        n.auth = (int)a.dot11DefaultAuthAlgorithm;
                        n.cipher = (int)a.dot11DefaultCipherAlgorithm;
                        n.connected = (a.dwFlags & WLAN_AVAILABLE_NETWORK_CONNECTED) != 0;
                        n.hasProfile = (a.dwFlags & WLAN_AVAILABLE_NETWORK_HAS_PROFILE) != 0;
                        if (n.hasProfile) n.profile = a.strProfileName;
                        auto it = std::find_if(nets_.begin(), nets_.end(), [&](const Net& x) { return x.ssid == n.ssid; });
                        if (it == nets_.end()) nets_.push_back(n);
                        else {   // the same network twice (with and without a profile): keep what helps most
                            if (n.connected) it->connected = true;
                            if (n.hasProfile && !it->hasProfile) { it->hasProfile = true; it->profile = n.profile; }
                            it->quality = std::max(it->quality, n.quality);
                        }
                    }
                    WlanFreeMemory(list);
                }
                std::stable_sort(nets_.begin(), nets_.end(), [](const Net& a, const Net& b) {
                    if (a.connected != b.connected) return a.connected;
                    return a.quality > b.quality;
                });
                if (nets_.size() > 8) nets_.resize(8);
                // the open row follows its network, not its place in the list (the list re-sorts by signal)
                if (expanded_ >= 0) {
                    auto it = std::find_if(nets_.begin(), nets_.end(), [&](const Net& n) { return n.ssid == expandedSsid_; });
                    if (it == nets_.end()) { expanded_ = -1; pwActive_ = false; }
                    else expanded_ = (int)(it - nets_.begin());
                }
            }
        }
        if (scan) {   // the radios and paired devices: read once per opening
            bts_ = PairedBluetooth();
            if (!radioBusy_) {
                radioBusy_ = true;
                HWND h = hwnd_;
                std::thread([h] { RadioJob(h, 0, 0); }).detach();
            }
        }
    }

    // ---- actions ----
    void SetDefaultDevice(const std::wstring& id) {
        ComPtr<IPolicyConfigAbi> pc;
        if (FAILED(CoCreateInstance(CLSID_PolicyConfig, nullptr, CLSCTX_ALL, IID_PolicyConfig, reinterpret_cast<void**>(pc.Put()))) || !pc) {
            ShellExecuteW(nullptr, nullptr, L"ms-settings:sound", nullptr, nullptr, SW_SHOWNORMAL);
            return;
        }
        pc->SetDefaultEndpoint(id.c_str(), eConsole);
        pc->SetDefaultEndpoint(id.c_str(), eMultimedia);
        pc->SetDefaultEndpoint(id.c_str(), eCommunications);
        LoadSound();   // the status icon follows by itself (default device notification)
    }

    void SetAppVolume(int i, float v) {
        if (i < 0 || i >= (int)sessions_.size()) return;
        Session& s = sessions_[i];
        s.vol = std::clamp(v, 0.0f, 1.0f);
        for (auto& sv : s.vols) {
            sv->SetMasterVolume(s.vol, nullptr);
            if (s.muted && s.vol > 0) sv->SetMute(FALSE, nullptr);
        }
        if (s.vol > 0) s.muted = false;
    }

    void ToggleAppMute(int i) {
        if (i < 0 || i >= (int)sessions_.size()) return;
        Session& s = sessions_[i];
        s.muted = !s.muted;
        for (auto& sv : s.vols) sv->SetMute(s.muted, nullptr);
    }

    static bool NeedsPassword(const Net& n) { return n.auth == 4 || n.auth == 7 || n.auth == 9; }       // WPA-PSK, WPA2-PSK, WPA3-SAE
    static bool Enterprise(const Net& n) { return n.auth == 3 || n.auth == 6 || n.auth == 8 || n.auth == 11; }

    std::wstring ProfileXml(const Net& n, const std::wstring& key) {
        wchar_t hex[3];
        std::wstring h;
        for (ULONG i = 0; i < n.raw.uSSIDLength; ++i) { swprintf(hex, 3, L"%02X", n.raw.ucSSID[i]); h += hex; }
        const wchar_t* auth = n.auth == 9 ? L"WPA3SAE" : n.auth == 4 ? L"WPAPSK" : n.auth == 7 ? L"WPA2PSK" : L"open";
        const wchar_t* enc = n.auth == 1 ? L"none" : n.cipher == 2 ? L"TKIP" : L"AES";
        std::wstring x = L"<?xml version=\"1.0\"?><WLANProfile xmlns=\"http://www.microsoft.com/networking/WLAN/profile/v1\"><name>" + XmlEscape(n.ssid) +
                         L"</name><SSIDConfig><SSID><hex>" + h + L"</hex><name>" + XmlEscape(n.ssid) +
                         L"</name></SSID></SSIDConfig><connectionType>ESS</connectionType><connectionMode>auto</connectionMode><MSM><security><authEncryption><authentication>" +
                         auth + L"</authentication><encryption>" + enc + L"</encryption><useOneX>false</useOneX></authEncryption>";
        if (!key.empty())
            x += L"<sharedKey><keyType>passPhrase</keyType><protected>false</protected><keyMaterial>" + XmlEscape(key) + L"</keyMaterial></sharedKey>";
        x += L"</security></MSM></WLANProfile>";
        return x;
    }

    void Connect(int i) {
        if (!wlan_ || i < 0 || i >= (int)nets_.size()) return;
        Net n = nets_[i];
        std::wstring profile = n.profile;
        if (!n.hasProfile) {
            if (Enterprise(n)) { Hide(); ShellExecuteW(nullptr, nullptr, L"ms-settings:network-wifi", nullptr, nullptr, SW_SHOWNORMAL); return; }
            if (NeedsPassword(n) && password_.size() < 8) { message_ = TR(L"Passordet må ha minst 8 tegn.", L"The password needs at least 8 characters."); return; }
            DWORD reason = 0;
            std::wstring xml = ProfileXml(n, NeedsPassword(n) ? password_ : L"");
            if (WlanSetProfile(wlan_, &iface_, 0, xml.c_str(), nullptr, TRUE, nullptr, &reason) != ERROR_SUCCESS) {
                message_ = TR(L"Windows godtok ikke nettverket. Prøv Nettverksinnstillinger.", L"Windows didn't accept the network. Try Network settings.");
                return;
            }
            profile = n.ssid;
            madeProfile_ = profile;
        } else {
            madeProfile_.clear();
        }
        WLAN_CONNECTION_PARAMETERS p{};
        p.wlanConnectionMode = wlan_connection_mode_profile;
        p.strProfile = profile.c_str();
        p.dot11BssType = dot11_BSS_type_any;
        if (WlanConnect(wlan_, &iface_, &p, nullptr) != ERROR_SUCCESS) {
            message_ = TR(L"Kunne ikke koble til.", L"Couldn't connect.");
            return;
        }
        connecting_ = n.ssid;
        message_ = TR(L"Kobler til …", L"Connecting …");
        pwActive_ = false;
        password_.clear();
        SetTimer(hwnd_, T_CONNECT, 15000, nullptr);
    }

    // Is Windows connected to that network right now? (asked directly, not from what the panel last saw)
    bool ConnectedTo(const std::wstring& ssid) {
        if (!wlan_ || ssid.empty()) return false;
        DWORD size = 0;
        WLAN_CONNECTION_ATTRIBUTES* ca = nullptr;
        bool yes = false;
        if (WlanQueryInterface(wlan_, &iface_, wlan_intf_opcode_current_connection, nullptr, &size, (PVOID*)&ca, nullptr) == ERROR_SUCCESS && ca) {
            const DOT11_SSID& d = ca->wlanAssociationAttributes.dot11Ssid;
            std::wstring now = Utf8(d.ucSSID, std::min<ULONG>(d.uSSIDLength, 32));
            yes = ca->isState == wlan_interface_state_connected && now == ssid;
            WlanFreeMemory(ca);
        }
        if (!yes) {   // or as the status monitor has it
            const StatusState& st = g_app.status->State();
            yes = st.net == StatusState::Wifi && st.ssid == ssid;
        }
        return yes;
    }

    void ConnectTimedOut() {
        KillTimer(hwnd_, T_CONNECT);
        if (connecting_.empty()) return;
        if (ConnectedTo(connecting_)) {   // it worked (maybe while the panel was closed): keep the profile
            connecting_.clear();
            madeProfile_.clear();
            message_.clear();
            Place(false);
            return;
        }
        connecting_.clear();
        // a profile we just made with a wrong password shouldn't stay behind
        if (!madeProfile_.empty() && wlan_) WlanDeleteProfile(wlan_, &iface_, madeProfile_.c_str(), nullptr);
        madeProfile_.clear();
        message_ = TR(L"Kunne ikke koble til. Feil passord?", L"Couldn't connect. Wrong password?");
        Place(false);
    }

    void Disconnect() {
        if (wlan_) WlanDisconnect(wlan_, &iface_, nullptr);
        expanded_ = -1;
    }

    void SetRadio(int kind, bool on) {
        if (radioBusy_) return;
        radioBusy_ = true;
        if (kind == 1) wifiRadio_ = on ? 1 : 2;   // show the new position at once
        else btRadio_ = on ? 1 : 2;
        HWND h = hwnd_;
        std::thread([h, kind, on] { RadioJob(h, kind, on ? 1 : 2); }).detach();
    }

    // ---- window ----
    bool Create() {
        if (hwnd_) return true;
        static bool reg = false;
        if (!reg) {
            WNDCLASSEXW wc{sizeof wc};
            wc.lpfnWndProc = Proc;
            wc.hInstance = g_app.hinst;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.lpszClassName = L"SkylineFlyout";
            RegisterClassExW(&wc);
            reg = true;
        }
        hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"SkylineFlyout", L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, g_app.hinst, this);
        if (!hwnd_) return false;
        BOOL dark = g_app.theme.dark;
        DwmSetWindowAttribute(hwnd_, 20, &dark, sizeof dark);
        MARGINS m{-1, -1, -1, -1};
        DwmExtendFrameIntoClientArea(hwnd_, &m);
        DWORD corner = 2;   // round
        DwmSetWindowAttribute(hwnd_, 33, &corner, sizeof corner);
        BOOL exclude = TRUE;
        DwmSetWindowAttribute(hwnd_, DWMWA_EXCLUDED_FROM_PEEK, &exclude, sizeof exclude);
        ApplyGlassEffect(hwnd_, Part::Flyout);
        return true;
    }

    void DropResources() {
        rt_.Reset();
        brush_.Reset();
        fTitle_.Reset(); fText_.Reset(); fSub_.Reset(); fRight_.Reset(); fGlyph_.Reset(); fGlyphS_.Reset(); fCenter_.Reset(); fCenterS_.Reset();
        appBmps_.clear();
        for (auto& f : stack_) f.bmp.Reset();
    }

    // Size from the content, under the icon, inside the screen
    void Place(bool show) {
        draw_ = false;
        float h = Build();
        int w = MulDiv((int)kW, dpi_, 96), hp = (int)std::ceil(h * dpi_ / 96.0f);
        MONITORINFO mi{sizeof mi};
        GetMonitorInfoW(MonitorFromRect(&anchor_, MONITOR_DEFAULTTONEAREST), &mi);
        int gap = MulDiv(6, dpi_, 96), edge = MulDiv(8, dpi_, 96);
        int cx = (anchor_.left + anchor_.right) / 2;
        int left = std::clamp(cx - w / 2, (int)mi.rcMonitor.left + edge, (int)mi.rcMonitor.right - w - edge);
        // the stack (and anything opened from the bottom of the screen) opens upwards
        bool up = kind_ == Flyouts::Stack || anchor_.top > (mi.rcMonitor.top + mi.rcMonitor.bottom) / 2;
        int top = up ? anchor_.top - gap - hp : anchor_.bottom + gap;
        UINT flags = SWP_NOACTIVATE | (show ? SWP_SHOWWINDOW : 0);
        SetWindowPos(hwnd_, HWND_TOPMOST, left, top, w, hp, flags);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    ComPtr<IDWriteTextFormat> Fmt(const wchar_t* fam, float size, DWRITE_FONT_WEIGHT w, DWRITE_TEXT_ALIGNMENT al) {
        ComPtr<IDWriteTextFormat> f;
        g_gfx.dwrite->CreateTextFormat(fam, nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"", f.Put());
        if (!f) return f;
        f->SetTextAlignment(al);
        f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        ComPtr<IDWriteInlineObject> ell;
        g_gfx.dwrite->CreateEllipsisTrimmingSign(f.Get(), ell.Put());
        DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        f->SetTrimming(&trim, ell.Get());
        return f;
    }

    bool EnsureResources() {
        if (!fText_) {
            fTitle_ = Fmt(L"Segoe UI", 15, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING);
            fText_ = Fmt(L"Segoe UI", 13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING);
            fSub_ = Fmt(L"Segoe UI", 11.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING);
            fRight_ = Fmt(L"Segoe UI", 12, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING);
            fGlyph_ = Fmt(GlyphFont(), 16, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
            fGlyphS_ = Fmt(GlyphFont(), 12, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
            fCenter_ = Fmt(L"Segoe UI", 13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
            fCenterS_ = Fmt(L"Segoe UI", 11, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        if (rt_) return true;
        RECT rc;
        GetClientRect(hwnd_, &rc);
        auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                                  D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), (float)dpi_, (float)dpi_);
        if (FAILED(g_gfx.d2d->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(rc.right, rc.bottom)), rt_.Put())))
            return false;
        rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        rt_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1), brush_.Put());
        appBmps_.clear();
        return true;
    }

    // ---- drawing primitives (skipped while measuring) ----
    void Text(const std::wstring& t, IDWriteTextFormat* f, D2D1_RECT_F r, D2D1_COLOR_F c) {
        if (!draw_ || t.empty() || !f) return;
        brush_->SetColor(c);
        rt_->DrawTextW(t.c_str(), (UINT32)t.size(), f, r, brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    void Glyph(wchar_t g, IDWriteTextFormat* f, D2D1_RECT_F r, D2D1_COLOR_F c) { Text(std::wstring(1, g), f, r, c); }
    void Fill(D2D1_RECT_F r, float rad, D2D1_COLOR_F c) {
        if (!draw_) return;
        brush_->SetColor(c);
        rt_->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), brush_.Get());
    }
    int AddHit(D2D1_RECT_F r, HitKind k, int idx) { hits_.push_back({r, k, idx}); return (int)hits_.size() - 1; }
    bool Hot(int i) const { return i == hover_ || i == drag_; }

    // A slider: the track, the filled part and a knob. Returns its hit index.
    int Slider(D2D1_RECT_F r, float v, HitKind k, int idx, bool dim) {
        const Theme& th = g_app.theme;
        int me = AddHit(D2D1::RectF(r.left - 6, r.top - 8, r.right + 6, r.bottom + 8), k, idx);
        float cy = (r.top + r.bottom) / 2, x = r.left + (r.right - r.left) * std::clamp(v, 0.0f, 1.0f);
        Fill(D2D1::RectF(r.left, cy - 2, r.right, cy + 2), 2, A(th.fg, 0.18f));
        Fill(D2D1::RectF(r.left, cy - 2, x, cy + 2), 2, dim ? A(th.fg, 0.45f) : th.accent);
        if (draw_) {
            float kr = Hot(me) ? 8.0f : 7.0f;
            brush_->SetColor(g_app.theme.dark ? D2D1::ColorF(0.95f, 0.95f, 0.96f) : D2D1::ColorF(1, 1, 1));
            rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, cy), kr, kr), brush_.Get());
            brush_->SetColor(A(D2D1::ColorF(0, 0, 0), 0.25f));
            rt_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(x, cy), kr, kr), brush_.Get(), 0.8f);
        }
        return me;
    }

    // An on/off switch like Windows'
    void Switch(D2D1_RECT_F r, bool on, bool known, HitKind k) {
        const Theme& th = g_app.theme;
        int me = AddHit(D2D1::RectF(r.left - 4, r.top - 4, r.right + 4, r.bottom + 4), k, 0);
        if (!draw_) return;
        float rad = (r.bottom - r.top) / 2;
        if (on) Fill(r, rad, th.accent);
        else {
            Fill(r, rad, A(th.fg, Hot(me) ? 0.12f : 0.06f));
            brush_->SetColor(A(th.fg, known ? 0.55f : 0.25f));
            rt_->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f), rad, rad), brush_.Get(), 1);
        }
        float kx = on ? r.right - rad : r.left + rad;
        brush_->SetColor(on ? (th.dark ? D2D1::ColorF(0, 0, 0, 0.85f) : D2D1::ColorF(1, 1, 1)) : A(th.fg, known ? 0.8f : 0.35f));
        rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(kx, (r.top + r.bottom) / 2), rad - 4.5f, rad - 4.5f), brush_.Get());
    }

    void Separator(float& y) {
        Fill(D2D1::RectF(kPad, y + 6, kW - kPad, y + 7), 0, g_app.theme.hairline);
        y += 13;
    }

    // A clickable text row at the bottom ("Sound settings ›")
    void Link(float& y, const wchar_t* text, int idx) {
        const Theme& th = g_app.theme;
        D2D1_RECT_F r = D2D1::RectF(8, y, kW - 8, y + 32);
        int me = AddHit(r, HLink, idx);
        if (Hot(me)) Fill(r, 8, th.hover);
        Text(text, fText_.Get(), D2D1::RectF(r.left + 10, r.top, r.right - 30, r.bottom), th.fg);
        Glyph(0xE76C, fGlyphS_.Get(), D2D1::RectF(r.right - 28, r.top, r.right - 6, r.bottom), th.fgDim);
        y += 32;
    }

    wchar_t VolumeGlyph(float v, bool muted) const {
        if (muted) return 0xE74F;
        return v <= 0.001f ? 0xE992 : v < 0.34f ? 0xE993 : v < 0.67f ? 0xE994 : 0xE995;
    }

    // Lays out (and, when drawing, paints) the whole panel. Returns its height in DIPs.
    float Build() {
        hits_.clear();
        return kind_ == Flyouts::Sound ? BuildSound() : kind_ == Flyouts::Network ? BuildNetwork()
             : kind_ == Flyouts::Stack ? BuildStack() : BuildCalendar();
    }

    // ---- the dock's stack: the newest files in a folder (read when it opens, icons as they come into view) ----
    struct StackFile { std::wstring path, name; ComPtr<IWICBitmapSource> img; ComPtr<ID2D1Bitmap> bmp; bool tried = false; };
    void LoadStack() {
        stack_.clear();
        stackScroll_ = 0;
        stackFolder_ = StackFolder();
        if (stackFolder_.empty()) return;
        std::vector<std::pair<ULONGLONG, StackFile>> all;
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileW((stackFolder_ + L"\\*").c_str(), &fd);
        if (f == INVALID_HANDLE_VALUE) return;
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
            std::wstring n = fd.cFileName;
            if (ToLower(n) == L"desktop.ini") continue;
            StackFile sf;
            sf.path = stackFolder_ + L"\\" + n;
            sf.name = n;
            all.push_back({((ULONGLONG)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime, std::move(sf)});
        } while (FindNextFileW(f, &fd));
        FindClose(f);
        std::sort(all.begin(), all.end(), [](auto& a, auto& b) { return a.first > b.first; });   // newest first, like the Mac
        stackTotal_ = all.size();
        for (auto& [t, sf] : all) {
            if (stack_.size() >= 60) break;
            stack_.push_back(std::move(sf));
        }
    }

    float BuildStack() {
        const Theme& th = g_app.theme;
        float y = 12;
        std::wstring title = FileNameOf(stackFolder_);
        PWSTR dl = nullptr;   // Downloads has a nicer name than its folder
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &dl)) && dl) {
            if (!lstrcmpiW(dl, stackFolder_.c_str())) title = TR(L"Nedlastinger", L"Downloads");
            CoTaskMemFree(dl);
        }
        Text(title, fTitle_.Get(), D2D1::RectF(kPad + 2, y, kW - 90, y + 24), th.fg);
        Text(std::to_wstring(stackTotal_) + TR(L" filer", L" files"), fRight_.Get(),
             D2D1::RectF(kW / 2, y, kW - kPad - 2, y + 24), th.fgDim);
        y += 34;
        const float cellW = (kW - 2 * kPad) / 4, cellH = 88;
        int rows = ((int)stack_.size() + 3) / 4;
        float viewH = std::min(4, std::max(rows, 1)) * cellH;
        stackMax_ = std::max(0.0f, rows * cellH - viewH);
        stackScroll_ = std::clamp(stackScroll_, 0.0f, stackMax_);
        if (stack_.empty()) {
            Text(TR(L"Mappen er tom.", L"The folder is empty."), fSub_.Get(), D2D1::RectF(kPad + 2, y, kW - kPad, y + 22), th.fgDim);
            y += 30;
        } else {
            float top = y, bottom = y + viewH;
            if (draw_) rt_->PushAxisAlignedClip(D2D1::RectF(0, top, kW, bottom), D2D1_ANTIALIAS_MODE_ALIASED);
            int px = MulDiv(40, dpi_, 96);
            for (int i = 0; i < (int)stack_.size(); ++i) {
                float cx = kPad + (i % 4) * cellW, cy = top + (i / 4) * cellH - stackScroll_;
                if (cy + cellH <= top || cy >= bottom) continue;
                D2D1_RECT_F r = D2D1::RectF(cx + 2, cy, cx + cellW - 2, cy + cellH - 4);
                int me = AddHit(D2D1::RectF(r.left, std::max(r.top, top), r.right, std::min(r.bottom, bottom)), HStackItem, i);
                if (Hot(me)) Fill(r, 10, th.hover);
                if (draw_) {
                    StackFile& sf = stack_[i];
                    if (!sf.tried) { sf.tried = true; sf.img = LoadAppImage(sf.path, px); }
                    if (!sf.bmp && sf.img) rt_->CreateBitmapFromWicBitmap(sf.img.Get(), nullptr, sf.bmp.Put());
                    float mx = (r.left + r.right) / 2;
                    if (sf.bmp) DrawImageHQ(rt_.Get(), sf.bmp.Get(), D2D1::RectF(mx - 20, r.top + 8, mx + 20, r.top + 48));
                }
                Text(stack_[i].name, fCenterS_.Get(), D2D1::RectF(r.left + 3, r.top + 52, r.right - 3, r.top + 70), th.fg);
            }
            if (draw_) rt_->PopAxisAlignedClip();
            if (stackScroll_ < stackMax_ - 1) Glyph(0xE70D, fGlyphS_.Get(), D2D1::RectF(kW / 2 - 12, bottom - 14, kW / 2 + 12, bottom), th.fgDim);
            y = bottom + 4;
        }
        Separator(y);
        Link(y, TR(L"Åpne i Utforsker", L"Open in File Explorer"), 6);
        return y + 8;
    }

    // ---- calendar ----
    static ULONGLONG Ft(const SYSTEMTIME& t) { FILETIME f; SystemTimeToFileTime(&t, &f); return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime; }
    static SYSTEMTIME St(ULONGLONG v) { FILETIME f{(DWORD)v, (DWORD)(v >> 32)}; SYSTEMTIME t; FileTimeToSystemTime(&f, &t); return t; }
    static int MondayIndex(const SYSTEMTIME& t) { return (t.wDayOfWeek + 6) % 7; }   // 0 = Monday
    static int IsoWeek(SYSTEMTIME t) {
        // the week's Thursday decides the year; week 1 holds the first Thursday
        ULONGLONG day = 864000000000ULL;
        ULONGLONG thu = Ft(t) + (ULONGLONG)(3 - MondayIndex(t)) * day;   // may step back: unsigned wrap is fine for addition
        SYSTEMTIME th = St(thu);
        SYSTEMTIME jan1{};
        jan1.wYear = th.wYear; jan1.wMonth = 1; jan1.wDay = 1;
        return (int)((Ft(th) - Ft(jan1)) / day) / 7 + 1;
    }
    std::wstring DateText(const SYSTEMTIME& t, const wchar_t* no, const wchar_t* en) {
        wchar_t b[128] = {};
        GetDateFormatEx(g_norsk ? L"nb-NO" : L"en-US", 0, &t, g_norsk ? no : en, b, 128, nullptr);
        std::wstring s = b;
        if (!s.empty()) s[0] = (wchar_t)towupper(s[0]);
        return s;
    }
    void StepMonth(int d) {
        int m = calM_ - 1 + d;
        calY_ += (m < 0 ? m - 11 : m) / 12;
        calM_ = ((m % 12) + 12) % 12 + 1;
    }

    float BuildCalendar() {
        const Theme& th = g_app.theme;
        SYSTEMTIME today;
        GetLocalTime(&today);
        float y = 12;
        Text(DateText(today, L"dddd d. MMMM", L"dddd, MMMM d"), fTitle_.Get(), D2D1::RectF(kPad + 2, y, kW - kPad, y + 24), th.fg);
        y += 25;
        Text(std::wstring(TR(L"Uke ", L"Week ")) + std::to_wstring(IsoWeek(today)), fSub_.Get(), D2D1::RectF(kPad + 2, y, kW - kPad, y + 18), th.fgDim);
        y += 24;
        Separator(y);
        // the month shown, with back / forward
        SYSTEMTIME first{};
        first.wYear = (WORD)calY_; first.wMonth = (WORD)calM_; first.wDay = 1;
        first = St(Ft(first));   // fills in the weekday
        bool thisMonth = calY_ == today.wYear && calM_ == today.wMonth;
        D2D1_RECT_F mr = D2D1::RectF(8, y, kW - 84, y + 32);
        int me = AddHit(mr, HCalToday, 0);
        if (Hot(me) && !thisMonth) Fill(mr, 8, th.hover);
        Text(DateText(first, L"MMMM yyyy", L"MMMM yyyy"), fText_.Get(), D2D1::RectF(mr.left + 8, mr.top, mr.right - 8, mr.bottom), th.fg);
        for (int i = 0; i < 2; ++i) {
            D2D1_RECT_F b = D2D1::RectF(kW - kPad - 66 + i * 34, y + 1, kW - kPad - 36 + i * 34, y + 31);
            int bm = AddHit(b, i ? HCalNext : HCalPrev, 0);
            if (Hot(bm)) Fill(b, 8, th.hover);
            Glyph(i ? 0xE76C : 0xE76B, fGlyphS_.Get(), b, th.fg);
        }
        y += 38;
        // weekdays (the week starts the way Windows is set up), with week numbers in their own column
        wchar_t fd[4] = {};
        GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_IFIRSTDAYOFWEEK, fd, 4);
        int firstDay = std::clamp(_wtoi(fd), 0, 6);   // 0 = Monday
        float gx = kPad + 30, cw = (kW - kPad - gx) / 7, rh = 34;
        for (int d = 0; d < 7; ++d) {
            wchar_t nm[16] = {};
            GetLocaleInfoEx(g_norsk ? L"nb-NO" : L"en-US", LOCALE_SSHORTESTDAYNAME1 + (firstDay + d) % 7, nm, 16);
            Text(nm, fCenterS_.Get(), D2D1::RectF(gx + d * cw, y, gx + (d + 1) * cw, y + 20), th.fgDim);
        }
        Text(TR(L"uke", L"wk"), fCenterS_.Get(), D2D1::RectF(kPad - 6, y, gx - 2, y + 20), A(th.fgDim, 0.7f));
        y += 24;
        int offset = (MondayIndex(first) - firstDay + 7) % 7;
        ULONGLONG day = 864000000000ULL, start = Ft(first) - (ULONGLONG)offset * day;
        for (int row = 0; row < 6; ++row) {
            float ry = y + row * rh;
            // the week number belongs to the row's Thursday
            for (int c = 0; c < 7; ++c) {
                SYSTEMTIME t = St(start + (ULONGLONG)(row * 7 + c) * day);
                if (MondayIndex(t) == 3)
                    Text(std::to_wstring(IsoWeek(t)), fCenterS_.Get(), D2D1::RectF(kPad - 6, ry, gx - 2, ry + rh), A(th.fgDim, 0.7f));
            }
            for (int c = 0; c < 7; ++c) {
                SYSTEMTIME t = St(start + (ULONGLONG)(row * 7 + c) * day);
                bool in = t.wMonth == calM_, isToday = t.wYear == today.wYear && t.wMonth == today.wMonth && t.wDay == today.wDay;
                D2D1_RECT_F cell = D2D1::RectF(gx + c * cw, ry, gx + (c + 1) * cw, ry + rh);
                if (isToday && draw_) {
                    float cx = (cell.left + cell.right) / 2, cy = (cell.top + cell.bottom) / 2;
                    brush_->SetColor(th.accent);
                    rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 14, 14), brush_.Get());
                }
                D2D1_COLOR_F col = isToday ? (th.dark ? D2D1::ColorF(0, 0, 0, 0.9f) : D2D1::ColorF(1, 1, 1)) : in ? th.fg : A(th.fgDim, 0.6f);
                Text(std::to_wstring(t.wDay), fCenter_.Get(), cell, col);
            }
        }
        y += 6 * rh + 6;
        Separator(y);
        Link(y, TR(L"Varsler", L"Notifications"), 5);
        Link(y, TR(L"Dato og klokkeslett", L"Date & time"), 4);
        return y + 8;
    }

    float BuildSound() {
        const Theme& th = g_app.theme;
        const StatusState& st = g_app.status->State();
        float y = 12;
        Text(TR(L"Lyd", L"Sound"), fTitle_.Get(), D2D1::RectF(kPad + 2, y, kW - kPad, y + 24), th.fg);
        if (st.audioPresent)
            Text(std::to_wstring((int)std::lround(st.volume * 100)) + L" %", fRight_.Get(), D2D1::RectF(kW / 2, y, kW - kPad - 2, y + 24), th.fgDim);
        y += 32;
        if (!st.audioPresent) {
            Text(TR(L"Ingen lydenhet funnet.", L"No sound device found."), fSub_.Get(), D2D1::RectF(kPad + 2, y, kW - kPad, y + 22), th.fgDim);
            y += 26;
        } else {
            // master: mute button and slider
            D2D1_RECT_F mb = D2D1::RectF(kPad, y, kPad + 34, y + 34);
            int me = AddHit(mb, HMute, 0);
            Fill(mb, 17, Hot(me) ? A(th.fg, 0.16f) : A(th.fg, 0.08f));
            Glyph(VolumeGlyph(st.volume, st.muted), fGlyph_.Get(), mb, th.fg);
            Slider(D2D1::RectF(kPad + 50, y + 13, kW - kPad - 8, y + 21), st.volume, HMaster, 0, st.muted);
            y += 44;
        }
        // output devices
        if (devices_.size() > 1) {
            Separator(y);
            Text(TR(L"Spill av på", L"Play on"), fSub_.Get(), D2D1::RectF(kPad + 2, y, kW - kPad, y + 20), th.fgDim);
            y += 22;
            for (int i = 0; i < (int)devices_.size(); ++i) {
                D2D1_RECT_F r = D2D1::RectF(8, y, kW - 8, y + 34);
                int me = AddHit(r, HDevice, i);
                if (devices_[i].def) Fill(r, 8, A(th.accent, 0.16f));
                else if (Hot(me)) Fill(r, 8, th.hover);
                std::wstring low = ToLower(devices_[i].name);
                bool phones = low.find(L"headphone") != std::wstring::npos || low.find(L"hodetelefon") != std::wstring::npos ||
                              low.find(L"headset") != std::wstring::npos || low.find(L"airpods") != std::wstring::npos || low.find(L"buds") != std::wstring::npos;
                Glyph(phones ? 0xE7F6 : 0xE7F5, fGlyph_.Get(), D2D1::RectF(r.left + 6, r.top, r.left + 34, r.bottom), devices_[i].def ? th.accent : th.fgDim);
                Text(devices_[i].name, fText_.Get(), D2D1::RectF(r.left + 40, r.top, r.right - 30, r.bottom), th.fg);
                if (devices_[i].def) Glyph(0xE73E, fGlyphS_.Get(), D2D1::RectF(r.right - 28, r.top, r.right - 6, r.bottom), th.accent);
                y += 36;
            }
        }
        // volume per app
        if (!sessions_.empty()) {
            Separator(y);
            Text(TR(L"Apper", L"Apps"), fSub_.Get(), D2D1::RectF(kPad + 2, y, kW - kPad, y + 20), th.fgDim);
            y += 24;
            int px = MulDiv(22, dpi_, 96);
            for (int i = 0; i < (int)sessions_.size(); ++i) {
                Session& s = sessions_[i];
                D2D1_RECT_F ib = D2D1::RectF(kPad, y + 2, kPad + 30, y + 32);
                int me = AddHit(ib, HAppMute, i);
                if (Hot(me)) Fill(ib, 8, th.hover);
                D2D1_RECT_F ir = D2D1::RectF(ib.left + 4, ib.top + 4, ib.right - 4, ib.bottom - 4);
                if (draw_) {
                    ID2D1Bitmap* bmp = nullptr;
                    if (!s.system) {
                        auto it = appBmps_.find(s.key);
                        if (it == appBmps_.end()) {
                            ComPtr<ID2D1Bitmap> b;
                            HICON big = nullptr;
                            if (ExtractIconExW(s.path.c_str(), 0, &big, nullptr, 1) && big) { b = BitmapFromIcon(rt_.Get(), big); DestroyIcon(big); }
                            it = appBmps_.emplace(s.key, b).first;
                        }
                        bmp = it->second.Get();
                    }
                    if (bmp) DrawImageHQ(rt_.Get(), bmp, ir, s.muted ? 0.4f : 1.0f);
                    else Glyph(0xE7F5, fGlyph_.Get(), ib, s.muted ? th.fgDim : th.fg);
                    if (s.muted) Glyph(0xE74F, fGlyphS_.Get(), D2D1::RectF(ib.right - 12, ib.bottom - 12, ib.right + 2, ib.bottom + 2), th.red);
                }
                (void)px;
                Text(s.name, fSub_.Get(), D2D1::RectF(kPad + 40, y, kW - kPad - 50, y + 16), th.fg);
                Text(std::to_wstring((int)std::lround(s.vol * 100)) + L" %", fRight_.Get(), D2D1::RectF(kW - kPad - 60, y, kW - kPad - 2, y + 16), th.fgDim);
                Slider(D2D1::RectF(kPad + 44, y + 22, kW - kPad - 8, y + 28), s.vol, HAppVol, i, s.muted);
                y += 40;
            }
        }
        Separator(y);
        Link(y, TR(L"Lydinnstillinger", L"Sound settings"), 0);
        Link(y, TR(L"Volummikser", L"Volume mixer"), 1);
        return y + 8;
    }

    wchar_t WifiGlyph(int q) const { return q >= 75 ? 0xE701 : q >= 50 ? 0xE874 : q >= 25 ? 0xE873 : 0xE872; }

    float BuildNetwork() {
        const Theme& th = g_app.theme;
        const StatusState& st = g_app.status->State();
        float y = 12;
        Text(TR(L"Nettverk", L"Network"), fTitle_.Get(), D2D1::RectF(kPad + 2, y, kW - kPad, y + 24), th.fg);
        y += 32;
        // Ethernet
        if (st.net == StatusState::Ethernet) {
            Glyph(0xE839, fGlyph_.Get(), D2D1::RectF(kPad, y, kPad + 30, y + 34), th.accent);
            Text(L"Ethernet", fText_.Get(), D2D1::RectF(kPad + 40, y, kW - kPad, y + 18), th.fg);
            Text(TR(L"Tilkoblet", L"Connected"), fSub_.Get(), D2D1::RectF(kPad + 40, y + 17, kW - kPad, y + 34), th.fgDim);
            y += 40;
        }
        // Wi-Fi
        if (hasWifi_ || wifiRadio_ >= 0) {
            if (st.net == StatusState::Ethernet) Separator(y);
            bool on = wifiRadio_ == 1 || (wifiRadio_ < 0 && hasWifi_);
            Text(L"Wi-Fi", fText_.Get(), D2D1::RectF(kPad + 2, y, kW - 80, y + 28), th.fg);
            Switch(D2D1::RectF(kW - kPad - 44, y + 4, kW - kPad, y + 24), on, wifiRadio_ >= 0, HWifiToggle);
            y += 34;
            if (on) {
                if (wifiDenied_) {
                    D2D1_RECT_F r = D2D1::RectF(8, y, kW - 8, y + 50);
                    int me = AddHit(r, HLocation, 0);
                    Fill(r, 8, Hot(me) ? th.hover : A(th.fg, 0.05f));
                    Glyph(0xE81D, fGlyph_.Get(), D2D1::RectF(r.left + 6, r.top, r.left + 34, r.bottom), th.fgDim);
                    Text(TR(L"Windows krever posisjonstilgang", L"Windows needs location access"), fText_.Get(), D2D1::RectF(r.left + 40, r.top + 6, r.right - 8, r.top + 26), th.fg);
                    Text(TR(L"for å vise Wi-Fi-nettverk. Klikk for å slå på.", L"to show Wi-Fi networks. Click to turn it on."), fSub_.Get(),
                         D2D1::RectF(r.left + 40, r.top + 25, r.right - 8, r.bottom - 6), th.fgDim);
                    y += 56;
                } else if (nets_.empty()) {
                    Text(TR(L"Leter etter nettverk …", L"Looking for networks …"), fSub_.Get(), D2D1::RectF(kPad + 2, y, kW - kPad, y + 22), th.fgDim);
                    y += 26;
                }
                for (int i = 0; i < (int)nets_.size(); ++i) {
                    const Net& n = nets_[i];
                    bool open = expanded_ == i;
                    float rowH = 40;
                    D2D1_RECT_F r = D2D1::RectF(8, y, kW - 8, y + rowH);
                    int me = AddHit(r, HNet, i);
                    if (n.connected) Fill(D2D1::RectF(r.left, r.top, r.right, r.bottom + (open ? 44 : 0)), 8, A(th.accent, 0.14f));
                    else if (Hot(me) || open) Fill(D2D1::RectF(r.left, r.top, r.right, r.bottom + (open ? 44 : 0)), 8, th.hover);
                    Glyph(WifiGlyph(n.quality), fGlyph_.Get(), D2D1::RectF(r.left + 6, r.top, r.left + 34, r.bottom), n.connected ? th.accent : th.fg);
                    std::wstring sub = n.connected ? TR(L"Tilkoblet", L"Connected")
                                     : connecting_ == n.ssid ? TR(L"Kobler til …", L"Connecting …")
                                     : n.secured ? TR(L"Sikret", L"Secured") : TR(L"Åpent", L"Open");
                    Text(n.ssid, fText_.Get(), D2D1::RectF(r.left + 40, r.top + 3, r.right - 30, r.top + 22), th.fg);
                    Text(sub, fSub_.Get(), D2D1::RectF(r.left + 40, r.top + 21, r.right - 30, r.bottom - 3), th.fgDim);
                    if (n.secured) Glyph(0xE72E, fGlyphS_.Get(), D2D1::RectF(r.right - 28, r.top, r.right - 6, r.bottom), th.fgDim);
                    y += rowH;
                    if (open) {
                        // what you can do with it: disconnect, connect, or a password first
                        bool pw = !n.connected && !n.hasProfile && NeedsPassword(n);
                        float bx = kW - kPad - 110;
                        if (pw) {
                            D2D1_RECT_F f = D2D1::RectF(r.left + 10, y + 4, bx - 8, y + 36);
                            int fm = AddHit(f, HPassword, i);
                            Fill(f, 6, A(th.fg, pwActive_ ? 0.12f : Hot(fm) ? 0.09f : 0.06f));
                            if (draw_ && pwActive_) {
                                brush_->SetColor(th.accent);
                                rt_->DrawLine(D2D1::Point2F(f.left + 6, f.bottom - 1), D2D1::Point2F(f.right - 6, f.bottom - 1), brush_.Get(), 1.5f);
                            }
                            std::wstring shown = reveal_ ? password_ : std::wstring(password_.size(), L'\x2022');
                            if (password_.empty() && !pwActive_) Text(TR(L"Passord", L"Password"), fSub_.Get(), D2D1::RectF(f.left + 10, f.top, f.right - 30, f.bottom), th.fgDim);
                            else Text(shown + (pwActive_ && caretOn_ ? L"|" : L""), fText_.Get(), D2D1::RectF(f.left + 10, f.top, f.right - 30, f.bottom), th.fg);
                            D2D1_RECT_F eye = D2D1::RectF(f.right - 28, f.top, f.right - 4, f.bottom);
                            AddHit(eye, HReveal, i);
                            Glyph(0xE7B3, fGlyphS_.Get(), eye, reveal_ ? th.accent : th.fgDim);
                        }
                        D2D1_RECT_F b = D2D1::RectF(bx, y + 4, kW - kPad - 4, y + 36);
                        int bm = AddHit(b, HNetAction, i);
                        Fill(b, 6, n.connected ? A(th.fg, Hot(bm) ? 0.16f : 0.10f) : A(th.accent, Hot(bm) ? 1.0f : 0.85f));
                        Text(n.connected ? TR(L"Koble fra", L"Disconnect") : TR(L"Koble til", L"Connect"), fText_.Get(),
                             D2D1::RectF(b.left + 12, b.top, b.right - 4, b.bottom), n.connected ? th.fg : (th.dark ? D2D1::ColorF(0, 0, 0, 0.9f) : D2D1::ColorF(1, 1, 1)));
                        y += 44;
                    }
                    y += 2;
                }
                if (!message_.empty()) {
                    Text(message_, fSub_.Get(), D2D1::RectF(kPad + 2, y, kW - kPad, y + 22), connecting_.empty() ? th.red : th.fgDim);
                    y += 24;
                }
            }
        }
        // Bluetooth
        if (btRadio_ >= 0) {
            Separator(y);
            bool on = btRadio_ == 1;
            Text(L"Bluetooth", fText_.Get(), D2D1::RectF(kPad + 2, y, kW - 80, y + 28), th.fg);
            Switch(D2D1::RectF(kW - kPad - 44, y + 4, kW - kPad, y + 24), on, true, HBtToggle);
            y += 34;
            if (on) {
                for (int i = 0; i < (int)bts_.size(); ++i) {
                    D2D1_RECT_F r = D2D1::RectF(8, y, kW - 8, y + 36);
                    int me = AddHit(r, HBt, i);
                    if (Hot(me)) Fill(r, 8, th.hover);
                    Glyph(BtGlyph(bts_[i].cod), fGlyph_.Get(), D2D1::RectF(r.left + 6, r.top, r.left + 34, r.bottom), bts_[i].connected ? th.accent : th.fgDim);
                    Text(bts_[i].name, fText_.Get(), D2D1::RectF(r.left + 40, r.top, r.right - 90, r.bottom), th.fg);
                    if (bts_[i].connected) Text(TR(L"Tilkoblet", L"Connected"), fRight_.Get(), D2D1::RectF(r.right - 100, r.top, r.right - 10, r.bottom), th.fgDim);
                    y += 36;
                }
                if (bts_.empty()) {
                    Text(TR(L"Ingen parede enheter.", L"No paired devices."), fSub_.Get(), D2D1::RectF(kPad + 2, y, kW - kPad, y + 22), th.fgDim);
                    y += 26;
                }
            }
        }
        Separator(y);
        Link(y, TR(L"Nettverksinnstillinger", L"Network settings"), 2);
        if (btRadio_ >= 0) Link(y, TR(L"Bluetooth-innstillinger", L"Bluetooth settings"), 3);
        return y + 8;
    }

    void Paint() {
        if (!EnsureResources()) return;
        D2D1_SIZE_F sz = rt_->GetSize();
        rt_->BeginDraw();
        rt_->Clear(D2D1::ColorF(0, 0, 0, 0));
        if (D2D1_COLOR_F tint = GlassTint(Part::Flyout); tint.a > 0) {
            brush_->SetColor(tint);
            rt_->FillRectangle(D2D1::RectF(0, 0, sz.width, sz.height), brush_.Get());
        }
        PaintVibe(rt_.Get(), D2D1::RectF(0, 0, sz.width, sz.height), Part::Flyout);
        PaintLiquid(rt_.Get(), D2D1::RectF(0, 0, sz.width, sz.height), 8);
        draw_ = true;
        Build();
        draw_ = false;
        if (FAILED(rt_->EndDraw())) { rt_.Reset(); appBmps_.clear(); for (auto& f : stack_) f.bmp.Reset(); }
    }

    int HitAt(float x, float y) const {
        for (int i = (int)hits_.size() - 1; i >= 0; --i)
            if (In(hits_[i].r, x, y)) return i;
        return -1;
    }

    float SliderValue(const Hit& h, float x) const {
        float l = h.r.left + 6, r = h.r.right - 6;
        return std::clamp((x - l) / std::max(1.0f, r - l), 0.0f, 1.0f);
    }

    void DragTo(float x) {
        if (drag_ < 0 || drag_ >= (int)hits_.size()) return;
        const Hit& h = hits_[drag_];
        float v = SliderValue(h, x);
        if (h.k == HMaster) g_app.status->SetVolume(v), g_app.InvalidateAll();
        else if (h.k == HAppVol) SetAppVolume(h.idx, v);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void Click(const Hit& h, float x) {
        switch (h.k) {
        case HMute: g_app.status->ToggleMute(); g_app.InvalidateAll(); break;
        case HMaster: case HAppVol: break;   // handled as a drag
        case HDevice: if (h.idx < (int)devices_.size() && !devices_[h.idx].def) SetDefaultDevice(devices_[h.idx].id); break;
        case HAppMute: ToggleAppMute(h.idx); break;
        case HLink: {
            static const wchar_t* pages[] = {L"ms-settings:sound", L"ms-settings:apps-volume", L"ms-settings:network", L"ms-settings:bluetooth",
                                             L"ms-settings:dateandtime"};
            Hide();
            if (h.idx == 5) SendHotkey({VK_LWIN, 'N'});   // Windows' notification centre
            else if (h.idx == 6) ShellExecuteW(nullptr, L"open", stackFolder_.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            else ShellExecuteW(nullptr, nullptr, pages[std::clamp(h.idx, 0, 4)], nullptr, nullptr, SW_SHOWNORMAL);
            return;
        }
        case HLocation: Hide(); ShellExecuteW(nullptr, nullptr, L"ms-settings:privacy-location", nullptr, nullptr, SW_SHOWNORMAL); return;
        case HWifiToggle: SetRadio(1, wifiRadio_ != 1); break;
        case HBtToggle: SetRadio(3, btRadio_ != 1); break;
        case HNet:
            if (h.idx < (int)nets_.size()) {
                const Net& n = nets_[h.idx];
                if (expanded_ == h.idx) { expanded_ = -1; pwActive_ = false; }
                else {
                    expanded_ = h.idx;
                    expandedSsid_ = n.ssid;
                    message_.clear();
                    password_.clear();
                    pwActive_ = !n.connected && !n.hasProfile && NeedsPassword(n);
                    if (pwActive_) SetTimer(hwnd_, T_CARET, 530, nullptr);
                    // a network you've used before connects straight away; so does an open one
                    if (!n.connected && (n.hasProfile || (!n.secured && !Enterprise(n)))) Connect(h.idx);
                    else if (!n.connected && !n.hasProfile && Enterprise(n)) Connect(h.idx);
                }
            }
            break;
        case HNetAction:
            if (h.idx < (int)nets_.size()) {
                if (nets_[h.idx].connected) Disconnect();
                else Connect(h.idx);
            }
            break;
        case HPassword: pwActive_ = true; SetTimer(hwnd_, T_CARET, 530, nullptr); break;
        case HReveal: reveal_ = !reveal_; break;
        case HBt: Hide(); ShellExecuteW(nullptr, nullptr, L"ms-settings:bluetooth", nullptr, nullptr, SW_SHOWNORMAL); return;
        case HStackItem:
            if (h.idx < (int)stack_.size()) {
                std::wstring p = stack_[h.idx].path;
                Hide();
                ShellExecuteW(nullptr, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
            return;
        case HCalPrev: StepMonth(-1); break;
        case HCalNext: StepMonth(1); break;
        case HCalToday: { SYSTEMTIME t; GetLocalTime(&t); calY_ = t.wYear; calM_ = t.wMonth; break; }
        }
        (void)x;
        Place(false);
    }

    static LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR) reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        auto* self = reinterpret_cast<Panel*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (!self) return DefWindowProcW(h, msg, wp, lp);
        if (!self->hwnd_) self->hwnd_ = h;
        float x = GET_X_LPARAM(lp) * 96.0f / self->dpi_, y = GET_Y_LPARAM(lp) * 96.0f / self->dpi_;
        switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(h, &ps);
            self->Paint();
            EndPaint(h, &ps);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_SIZE: if (self->rt_) self->rt_->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp))); return 0;
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE) self->Hide();   // a click anywhere else closes it
            return 0;
        case WM_TIMER:
            if (wp == T_CONNECT) self->ConnectTimedOut();
            else if (wp == T_CARET) {
                if (!self->pwActive_) KillTimer(h, T_CARET);
                self->caretOn_ = !self->caretOn_;
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        case WM_FLY_RADIO:
            self->radioBusy_ = false;
            self->wifiRadio_ = (int)(INT_PTR)wp;
            self->btRadio_ = (int)(INT_PTR)lp;
            if (self->Visible()) {
                if (self->btRadio_ == 1) self->bts_ = PairedBluetooth();
                self->LoadNetwork(false);
                self->Place(false);
            }
            return 0;
        case WM_MOUSEMOVE: {
            if (!self->tracking_) {
                TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, h, 0};
                TrackMouseEvent(&t);
                self->tracking_ = true;
            }
            if (self->drag_ >= 0) { self->DragTo(x); return 0; }
            int hv = self->HitAt(x, y);
            if (hv != self->hover_) { self->hover_ = hv; InvalidateRect(h, nullptr, FALSE); }
            return 0;
        }
        case WM_MOUSELEAVE:
            self->tracking_ = false;
            if (self->hover_ >= 0) { self->hover_ = -1; InvalidateRect(h, nullptr, FALSE); }
            return 0;
        case WM_LBUTTONDOWN: {
            int hv = self->HitAt(x, y);
            if (hv >= 0 && (self->hits_[hv].k == HMaster || self->hits_[hv].k == HAppVol)) {
                self->drag_ = hv;
                SetCapture(h);
                self->DragTo(x);
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            if (self->drag_ >= 0) {
                self->drag_ = -1;
                ReleaseCapture();
                if (self->kind_ == Flyouts::Sound) self->LoadSound();
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            int hv = self->HitAt(x, y);
            if (hv >= 0) { Hit hit = self->hits_[hv]; self->Click(hit, x); }
            else if (self->pwActive_) { self->pwActive_ = false; InvalidateRect(h, nullptr, FALSE); }
            return 0;
        }
        case WM_MOUSEWHEEL: {
            if (self->kind_ == Flyouts::Stack) {
                self->stackScroll_ = std::clamp(self->stackScroll_ - GET_WHEEL_DELTA_WPARAM(wp) / (float)WHEEL_DELTA * 60.0f, 0.0f, self->stackMax_);
                self->hover_ = -1;
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            if (self->kind_ == Flyouts::Calendar) {   // the wheel flips months
                self->StepMonth(GET_WHEEL_DELTA_WPARAM(wp) > 0 ? -1 : 1);
                self->Place(false);
                return 0;
            }
            if (self->kind_ != Flyouts::Sound) return 0;
            POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(h, &p);
            float px = p.x * 96.0f / self->dpi_, py = p.y * 96.0f / self->dpi_;
            float step = GET_WHEEL_DELTA_WPARAM(wp) / (float)WHEEL_DELTA * 0.02f;
            int hv = self->HitAt(px, py);
            if (hv >= 0 && (self->hits_[hv].k == HAppVol || self->hits_[hv].k == HAppMute)) {
                int i = self->hits_[hv].idx;
                if (i < (int)self->sessions_.size()) self->SetAppVolume(i, self->sessions_[i].vol + step);
            } else {
                g_app.status->ChangeVolume(step);
                g_app.InvalidateAll();
            }
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) {
                if (self->pwActive_) { self->pwActive_ = false; self->expanded_ = -1; self->Place(false); }
                else self->Hide();
                return 0;
            }
            if (self->pwActive_) {
                if (wp == VK_RETURN) { self->Connect(self->expanded_); self->Place(false); }
                else if (wp == 'V' && (GetKeyState(VK_CONTROL) & 0x8000) && OpenClipboard(h)) {
                    if (HANDLE d = GetClipboardData(CF_UNICODETEXT)) {
                        if (auto* t = (const wchar_t*)GlobalLock(d)) {
                            for (const wchar_t* c = t; *c && self->password_.size() < 63; ++c)
                                if (*c >= 32) self->password_ += *c;
                            GlobalUnlock(d);
                        }
                    }
                    CloseClipboard();
                    InvalidateRect(h, nullptr, FALSE);
                }
                return 0;
            }
            if (self->kind_ == Flyouts::Sound && (wp == VK_UP || wp == VK_DOWN || wp == VK_RIGHT || wp == VK_LEFT)) {
                g_app.status->ChangeVolume(wp == VK_UP || wp == VK_RIGHT ? 0.02f : -0.02f);
                g_app.InvalidateAll();
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        case WM_CHAR:
            if (self->pwActive_) {
                if (wp == 8) { if (!self->password_.empty()) self->password_.pop_back(); }
                else if (wp >= 32 && self->password_.size() < 63) self->password_ += (wchar_t)wp;
                self->caretOn_ = true;
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        case WM_DESTROY:
            if (self->wlan_) { WlanCloseHandle(self->wlan_, nullptr); self->wlan_ = nullptr; }
            return 0;
        }
        return DefWindowProcW(h, msg, wp, lp);
    }

    HWND hwnd_ = nullptr;
    UINT dpi_ = 96;
    RECT anchor_{};
    Flyouts::Kind kind_ = Flyouts::Sound;
    DWORD hiddenAt_ = 0;
    bool draw_ = false, tracking_ = false;
    int hover_ = -1, drag_ = -1;
    std::vector<Hit> hits_;
    // sound
    std::vector<Device> devices_;
    std::vector<Session> sessions_;
    std::map<std::wstring, ComPtr<ID2D1Bitmap>> appBmps_;
    // network
    HANDLE wlan_ = nullptr;
    GUID iface_{};
    bool hasWifi_ = false, wifiDenied_ = false;
    std::vector<Net> nets_;
    std::vector<BtDev> bts_;
    int wifiRadio_ = -1, btRadio_ = -1;   // -1 unknown / none, 1 on, 2 off, 3 disabled
    bool radioBusy_ = false;
    int expanded_ = -1;
    std::wstring expandedSsid_;   // which network that open row is
    bool pwActive_ = false, caretOn_ = true, reveal_ = false;
    std::wstring password_, message_, connecting_, madeProfile_;
    // drawing
    ComPtr<ID2D1HwndRenderTarget> rt_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<IDWriteTextFormat> fTitle_, fText_, fSub_, fRight_, fGlyph_, fGlyphS_, fCenter_, fCenterS_;
    int calY_ = 2026, calM_ = 1;   // the month the calendar shows
    std::vector<StackFile> stack_;
    std::wstring stackFolder_;
    float stackScroll_ = 0, stackMax_ = 0;
    size_t stackTotal_ = 0;
};

Panel g_panel;

} // namespace

namespace Flyouts {

bool Enabled() { return g_app.settings.flyoutsEnabled; }

void Toggle(Kind kind, RECT anchor, UINT dpi) {
    if (g_panel.Visible()) {
        bool same = g_panel.Kind() == kind;
        g_panel.Hide();
        if (same) return;
    } else if (g_panel.Kind() == kind && GetTickCount() - g_panel.HiddenAt() < 300) {
        return;   // this very click closed it (the panel lost focus to the bar): leave it closed
    }
    g_panel.Show(kind, anchor, dpi);
}

void Hide() { g_panel.Hide(); }

void ShowStack(RECT tile, UINT dpi) { Toggle(Stack, tile, dpi); }
bool StackOpen() { return g_panel.Visible() && g_panel.Kind() == Stack; }

void StatusChanged() { g_panel.Refresh(); }

void ApplySettings() {
    g_panel.Hide();
    g_panel.Destroy();   // new glass/theme on the next opening
}

} // namespace Flyouts
