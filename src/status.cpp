// Skyline — Copyright © 2026 Yen-Zen & Cyber. All rights reserved. See LICENSE.txt.
// Status sources — all event driven, no polling loops.
//   volume  : IAudioEndpointVolume callback + default device changes
//   network : WLAN notifications + NotifyIpInterfaceChange
//   battery : WM_POWERBROADCAST (registered by the bar windows)
#include "app.h"
#include <initguid.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <winsock2.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <wlanapi.h>
#include <atomic>

namespace {

HWND g_notify = nullptr;
void Poke() { if (g_notify) PostMessageW(g_notify, WM_SKY_STATUS, 0, 0); }

// ---------------------------------------------------------------- audio COM
class VolumeCallback final : public IAudioEndpointVolumeCallback {
public:
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override { ULONG r = --ref_; if (!r) delete this; return r; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IAudioEndpointVolumeCallback)) {
            *out = static_cast<IAudioEndpointVolumeCallback*>(this); AddRef(); return S_OK;
        }
        *out = nullptr; return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA) override { Poke(); return S_OK; }
private:
    std::atomic<ULONG> ref_{1};
};

class DeviceCallback final : public IMMNotificationClient {
public:
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override { ULONG r = --ref_; if (!r) delete this; return r; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMMNotificationClient)) {
            *out = static_cast<IMMNotificationClient*>(this); AddRef(); return S_OK;
        }
        *out = nullptr; return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == eRender && role == eConsole && g_notify) PostMessageW(g_notify, WM_SKY_STATUS, 1, 0);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override {
        if (g_notify) PostMessageW(g_notify, WM_SKY_STATUS, 1, 0);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }
private:
    std::atomic<ULONG> ref_{1};
};

ComPtr<IMMDeviceEnumerator> g_enum;
ComPtr<IAudioEndpointVolume> g_vol;
VolumeCallback* g_volCb = nullptr;
DeviceCallback* g_devCb = nullptr;

// ---------------------------------------------------------------- network
HANDLE g_wlan = nullptr;
HANDLE g_ipNotify = nullptr;

void WINAPI WlanCallback(PWLAN_NOTIFICATION_DATA, PVOID) { Poke(); }
void WINAPI IpCallback(PVOID, PMIB_IPINTERFACE_ROW, MIB_NOTIFICATION_TYPE) { Poke(); }

} // namespace

bool StatusMonitor::Start(HWND notify) {
    notify_ = notify;
    g_notify = notify;

    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   __uuidof(IMMDeviceEnumerator), (void**)g_enum.Put()))) {
        g_devCb = new DeviceCallback();
        g_enum->RegisterEndpointNotificationCallback(g_devCb);
    }
    RefreshAudioDevice();

    DWORD negotiated = 0;
    if (WlanOpenHandle(2, nullptr, &negotiated, &g_wlan) == ERROR_SUCCESS) {
        WlanRegisterNotification(g_wlan, WLAN_NOTIFICATION_SOURCE_ACM | WLAN_NOTIFICATION_SOURCE_MSM,
                                 TRUE, WlanCallback, nullptr, nullptr, nullptr);
    }
    NotifyIpInterfaceChange(AF_UNSPEC, IpCallback, nullptr, FALSE, &g_ipNotify);

    Refresh();
    return true;
}

void StatusMonitor::Stop() {
    if (g_vol && g_volCb) g_vol->UnregisterControlChangeNotify(g_volCb);
    g_vol.Reset();
    if (g_volCb) { g_volCb->Release(); g_volCb = nullptr; }
    if (g_enum && g_devCb) g_enum->UnregisterEndpointNotificationCallback(g_devCb);
    if (g_devCb) { g_devCb->Release(); g_devCb = nullptr; }
    g_enum.Reset();
    if (g_ipNotify) { CancelMibChangeNotify2(g_ipNotify); g_ipNotify = nullptr; }
    if (g_wlan) { WlanCloseHandle(g_wlan, nullptr); g_wlan = nullptr; }
    g_notify = nullptr;
}

void StatusMonitor::Refresh() {
    RefreshVolume();
    RefreshNetwork();
    RefreshBattery();
}

// ---------------------------------------------------------------- volume
void StatusMonitor::RefreshAudioDevice() {
    if (g_vol && g_volCb) g_vol->UnregisterControlChangeNotify(g_volCb);
    g_vol.Reset();
    if (!g_enum) { s_.audioPresent = false; return; }
    ComPtr<IMMDevice> dev;
    if (FAILED(g_enum->GetDefaultAudioEndpoint(eRender, eConsole, dev.Put())) || !dev) {
        s_.audioPresent = false;
        return;
    }
    if (FAILED(dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, (void**)g_vol.Put()))) {
        s_.audioPresent = false;
        return;
    }
    if (!g_volCb) g_volCb = new VolumeCallback();
    g_vol->RegisterControlChangeNotify(g_volCb);
    RefreshVolume();
}

void StatusMonitor::RefreshVolume() {
    if (!g_vol) { s_.audioPresent = false; return; }
    float v = 0; BOOL m = FALSE;
    g_vol->GetMasterVolumeLevelScalar(&v);
    g_vol->GetMute(&m);
    s_.audioPresent = true;
    s_.volume = v;
    s_.muted = m != FALSE;
}

void StatusMonitor::ChangeVolume(float delta) {
    if (!g_vol) return;
    float v = std::clamp(s_.volume + delta, 0.0f, 1.0f);
    g_vol->SetMasterVolumeLevelScalar(v, nullptr);
    if (s_.muted && delta > 0) g_vol->SetMute(FALSE, nullptr);
    RefreshVolume();
}

void StatusMonitor::SetVolume(float v) {
    if (!g_vol) return;
    g_vol->SetMasterVolumeLevelScalar(std::clamp(v, 0.0f, 1.0f), nullptr);
    if (s_.muted && v > 0) g_vol->SetMute(FALSE, nullptr);
    RefreshVolume();
}

void StatusMonitor::ToggleMute() {
    if (!g_vol) return;
    g_vol->SetMute(!s_.muted, nullptr);
    RefreshVolume();
}

// ---------------------------------------------------------------- network
void StatusMonitor::RefreshNetwork() {
    s_.net = StatusState::Offline;
    s_.wifiQuality = 0;
    s_.ssid.clear();

    // Wi-Fi first
    if (g_wlan) {
        PWLAN_INTERFACE_INFO_LIST list = nullptr;
        if (WlanEnumInterfaces(g_wlan, nullptr, &list) == ERROR_SUCCESS && list) {
            for (DWORD i = 0; i < list->dwNumberOfItems; ++i) {
                auto& info = list->InterfaceInfo[i];
                if (info.isState != wlan_interface_state_connected) continue;
                PWLAN_CONNECTION_ATTRIBUTES attr = nullptr;
                DWORD size = 0;
                if (WlanQueryInterface(g_wlan, &info.InterfaceGuid, wlan_intf_opcode_current_connection,
                                       nullptr, &size, (PVOID*)&attr, nullptr) == ERROR_SUCCESS && attr) {
                    s_.net = StatusState::Wifi;
                    s_.wifiQuality = (int)attr->wlanAssociationAttributes.wlanSignalQuality;
                    auto& ss = attr->wlanAssociationAttributes.dot11Ssid;
                    int n = MultiByteToWideChar(CP_UTF8, 0, (const char*)ss.ucSSID, (int)ss.uSSIDLength, nullptr, 0);
                    s_.ssid.resize(n);
                    MultiByteToWideChar(CP_UTF8, 0, (const char*)ss.ucSSID, (int)ss.uSSIDLength, s_.ssid.data(), n);
                    WlanFreeMemory(attr);
                    break;
                }
            }
            WlanFreeMemory(list);
        }
    }
    if (s_.net == StatusState::Wifi) return;

    // Wired: an Ethernet adapter that is up and has a gateway
    ULONG size = 16 * 1024;
    std::vector<BYTE> buf(size);
    ULONG flags = GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG r = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, (PIP_ADAPTER_ADDRESSES)buf.data(), &size);
    if (r == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        r = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, (PIP_ADAPTER_ADDRESSES)buf.data(), &size);
    }
    if (r != NO_ERROR) return;
    for (auto a = (PIP_ADAPTER_ADDRESSES)buf.data(); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || !a->FirstGatewayAddress) continue;
        if (a->IfType == IF_TYPE_ETHERNET_CSMACD) { s_.net = StatusState::Ethernet; return; }
        if (a->IfType == IF_TYPE_IEEE80211) { s_.net = StatusState::Wifi; s_.wifiQuality = 100; }
    }
}

// ---------------------------------------------------------------- battery
void StatusMonitor::RefreshBattery() {
    SYSTEM_POWER_STATUS ps{};
    if (!GetSystemPowerStatus(&ps) || ps.BatteryFlag == 128 || ps.BatteryFlag == 255 || ps.BatteryLifePercent > 100) {
        s_.hasBattery = false;
        return;
    }
    s_.hasBattery = true;
    s_.batteryPct = ps.BatteryLifePercent;
    s_.onAc = ps.ACLineStatus == 1;
    s_.charging = (ps.BatteryFlag & 8) != 0;   // not "on AC": laptops with an 80 % charge limit sit idle on AC
}
