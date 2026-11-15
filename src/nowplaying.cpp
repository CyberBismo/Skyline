// Skyline — Copyright © 2026 Yen-Zen & Cyber. All rights reserved. See LICENSE.txt.
// Now Playing — see nowplaying.h
//
// Windows' media sessions are a WinRT API (Windows.Media.Control). Our toolchain
// has no projection headers for it, so the few interfaces we need are declared
// here by hand. Every object we get is QueryInterface'd for the exact interface
// before use: if anything doesn't match, the plugin simply shows nothing.
#include "nowplaying.h"
#include "app.h"
#include "applist.h"
#include "icons.h"
#include "dock.h"
#include <roapi.h>
#include <winstring.h>
#include <inspectable.h>
#include <shcore.h>
#include <shellscalingapi.h>
#include <cmath>
#include <algorithm>

namespace {

// ---------------------------------------------------------------------------
// WinRT ABI (Windows.Media.Control)
// ---------------------------------------------------------------------------
struct WinTimeSpan { INT64 ticks; };   // 100 ns
struct WinDateTime { INT64 ticks; };   // 100 ns since 1601 (same as FILETIME)

const GUID IID_AsyncInfo     = {0x00000036, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
const GUID IID_MgrStatics    = {0x2050C4EE, 0x11A0, 0x57DE, {0xAE, 0xD7, 0xC9, 0x7C, 0x70, 0x33, 0x82, 0x45}};
const GUID IID_Manager       = {0xCACE8EAC, 0xE86E, 0x504A, {0xAB, 0x31, 0x5F, 0xF8, 0xFF, 0x1B, 0xCE, 0x49}};
const GUID IID_Session       = {0x7148C835, 0x9B14, 0x5AE2, {0xAB, 0x85, 0xDC, 0x9B, 0x1C, 0x14, 0xE1, 0xA8}};
const GUID IID_MediaProps    = {0x68856CF6, 0xADB4, 0x54B2, {0xAC, 0x16, 0x05, 0x83, 0x79, 0x07, 0xAC, 0xB6}};
const GUID IID_Timeline      = {0xEDE34136, 0x6F25, 0x588D, {0x8E, 0xCF, 0xEA, 0x5B, 0x67, 0x35, 0xAA, 0xA5}};
const GUID IID_PlaybackInfo  = {0x94B4B6CF, 0xE8BA, 0x51AD, {0x87, 0xA7, 0xC1, 0x0A, 0xDE, 0x10, 0x61, 0x27}};
const GUID IID_StreamRef     = {0x33EE3134, 0x1DD6, 0x4E3A, {0x80, 0x67, 0xD1, 0xC1, 0x62, 0xE8, 0x64, 0x2B}};

struct IAsyncInfoAbi : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Id(UINT32*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Status(INT32*) = 0;   // 0 started, 1 completed, 2 canceled, 3 error
    virtual HRESULT STDMETHODCALLTYPE get_ErrorCode(HRESULT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE Cancel() = 0;
    virtual HRESULT STDMETHODCALLTYPE Close() = 0;
};
// IAsyncOperation<T> where T is an interface: GetResults(T**)
struct IAsyncOpAbi : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE put_Completed(void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Completed(void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetResults(IInspectable**) = 0;
};
struct IMgrStaticsAbi : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE RequestAsync(IInspectable** op) = 0;
};
struct IManagerAbi : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE GetCurrentSession(IInspectable** session) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetSessions(IInspectable** list) = 0;
};
struct ISessionAbi : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_SourceAppUserModelId(HSTRING*) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryGetMediaPropertiesAsync(IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetTimelineProperties(IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPlaybackInfo(IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryPlayAsync(IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryPauseAsync(IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryStopAsync(IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryRecordAsync(IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryFastForwardAsync(IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryRewindAsync(IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE TrySkipNextAsync(IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE TrySkipPreviousAsync(IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryChangeChannelUpAsync(IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryChangeChannelDownAsync(IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryTogglePlayPauseAsync(IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryChangeAutoRepeatModeAsync(INT32, IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryChangePlaybackRateAsync(double, IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryChangeShuffleActiveAsync(boolean, IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryChangePlaybackPositionAsync(INT64, IInspectable**) = 0;
};
struct IMediaPropsAbi : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Title(HSTRING*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Subtitle(HSTRING*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_AlbumArtist(HSTRING*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Artist(HSTRING*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_AlbumTitle(HSTRING*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_TrackNumber(INT32*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Genres(IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_AlbumTrackCount(INT32*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_PlaybackType(IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Thumbnail(IInspectable**) = 0;
};
struct ITimelineAbi : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_StartTime(WinTimeSpan*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_EndTime(WinTimeSpan*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_MinSeekTime(WinTimeSpan*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_MaxSeekTime(WinTimeSpan*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Position(WinTimeSpan*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_LastUpdatedTime(WinDateTime*) = 0;
};
struct IPlaybackInfoAbi : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Controls(IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_PlaybackStatus(INT32*) = 0;   // 4 playing, 5 paused
};
struct IStreamRefAbi : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE OpenReadAsync(IInspectable** op) = 0;
};

template <class T> ComPtr<T> As(IUnknown* p, const GUID& iid) {
    ComPtr<T> out;
    if (p) p->QueryInterface(iid, reinterpret_cast<void**>(out.Put()));
    return out;
}

std::wstring Str(HSTRING h) {
    if (!h) return L"";
    UINT32 n = 0;
    const wchar_t* p = WindowsGetStringRawBuffer(h, &n);
    std::wstring s(p ? p : L"", n);
    WindowsDeleteString(h);
    return s;
}

// 0 pending, 1 done (result in *out), -1 failed
int AsyncResult(IInspectable* op, ComPtr<IInspectable>& out) {
    auto info = As<IAsyncInfoAbi>(op, IID_AsyncInfo);
    if (!info) return -1;
    INT32 st = 0;
    if (FAILED(info->get_Status(&st))) return -1;
    if (st == 0) return 0;
    if (st != 1) return -1;
    IInspectable* r = nullptr;
    if (FAILED(static_cast<IAsyncOpAbi*>(op)->GetResults(&r)) || !r) return -1;
    out.Attach(r);
    return 1;
}

// ---------------------------------------------------------------------------
// The model
// ---------------------------------------------------------------------------
NowPlaying::State g_state;
ComPtr<IMgrStaticsAbi> g_statics;
ComPtr<IInspectable> g_mgrOp, g_propsOp, g_thumbOp;
ComPtr<IManagerAbi> g_mgr;
std::wstring g_appId, g_trackKey;
DWORD g_lastProps = 0;
UINT_PTR g_timer = 0;
bool g_failed = false;
// timeline as last reported (to extrapolate the position between reports)
INT64 g_posTicks = 0, g_endTicks = 0, g_updated = 0, g_startTicks = 0;
UINT g_appGen = 0;
UINT g_interval = 0;   // current poll interval
ComPtr<IWICBitmapSource> g_appIcon;
int g_appIconPx = 0;

void Notify(bool layout);

ComPtr<IWICBitmapSource> DecodeArt(IInspectable* streamWithType) {
    ComPtr<IWICBitmapSource> out;
    ComPtr<IStream> st;
    if (FAILED(CreateStreamOverRandomAccessStream(streamWithType, IID_PPV_ARGS(st.Put())))) return out;
    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(g_gfx.wic->CreateDecoderFromStream(st.Get(), nullptr, WICDecodeMetadataCacheOnLoad, dec.Put()))) return out;
    ComPtr<IWICBitmapFrameDecode> fr;
    if (FAILED(dec->GetFrame(0, fr.Put()))) return out;
    UINT w = 0, h = 0;
    fr->GetSize(&w, &h);
    if (!w || !h) return out;
    // Square crop (some apps send 16:9 video thumbnails)
    UINT side = std::min(w, h);
    ComPtr<IWICBitmapClipper> clip;
    g_gfx.wic->CreateBitmapClipper(clip.Put());
    WICRect r{(INT)(w - side) / 2, (INT)(h - side) / 2, (INT)side, (INT)side};
    IWICBitmapSource* src = fr.Get();
    if (clip && SUCCEEDED(clip->Initialize(fr.Get(), &r))) src = clip.Get();
    ComPtr<IWICFormatConverter> conv;
    g_gfx.wic->CreateFormatConverter(conv.Put());
    if (!conv || FAILED(conv->Initialize(src, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
        return out;
    UINT px = std::min<UINT>(side, 320);
    ComPtr<IWICBitmapScaler> sc;
    g_gfx.wic->CreateBitmapScaler(sc.Put());
    if (!sc || (FAILED(sc->Initialize(conv.Get(), px, px, (WICBitmapInterpolationMode)4)) &&
                FAILED(sc->Initialize(conv.Get(), px, px, WICBitmapInterpolationModeFant))))
        return out;
    ComPtr<IWICBitmap> bmp;
    if (SUCCEEDED(g_gfx.wic->CreateBitmapFromSource(sc.Get(), WICBitmapCacheOnLoad, bmp.Put()))) bmp->QueryInterface(IID_PPV_ARGS(out.Put()));
    return out;
}

D2D1_COLOR_F AverageColor(IWICBitmapSource* img) {
    D2D1_COLOR_F c{0, 0, 0, 0};
    ComPtr<IWICBitmapScaler> sc;
    g_gfx.wic->CreateBitmapScaler(sc.Put());
    if (!sc || FAILED(sc->Initialize(img, 4, 4, WICBitmapInterpolationModeFant))) return c;
    BYTE px[4 * 4 * 4];
    if (FAILED(sc->CopyPixels(nullptr, 16, sizeof px, px))) return c;
    float r = 0, g = 0, b = 0;
    for (int i = 0; i < 16; ++i) { b += px[i * 4]; g += px[i * 4 + 1]; r += px[i * 4 + 2]; }
    r /= 16 * 255.0f; g /= 16 * 255.0f; b /= 16 * 255.0f;
    // A little more colour, so greyish covers still give a hint
    float l = (r + g + b) / 3;
    r = std::clamp(l + (r - l) * 1.5f, 0.0f, 1.0f);
    g = std::clamp(l + (g - l) * 1.5f, 0.0f, 1.0f);
    b = std::clamp(l + (b - l) * 1.5f, 0.0f, 1.0f);
    return D2D1::ColorF(r, g, b, 1);
}

std::wstring AppNameFor(const std::wstring& id) {
    if (id.empty()) return L"";
    std::wstring l = ToLower(id);
    for (const auto& a : AllApps(0xFFFFFFFF)) {
        if (a.setting) continue;
        if (a.key.size() >= l.size() && a.key.compare(a.key.size() - l.size(), l.size(), l) == 0) return a.name;
        if (!a.exe.empty() && ToLower(FileNameOf(a.exe)) == l) return a.name;
    }
    std::wstring n = id;
    size_t bang = n.find(L'!');
    if (bang != std::wstring::npos) n = n.substr(bang + 1);
    if (n.size() > 4 && ToLower(n.substr(n.size() - 4)) == L".exe") n.resize(n.size() - 4);
    if (!n.empty()) n[0] = (wchar_t)towupper(n[0]);
    return n;
}

void Clear() {
    bool was = g_state.active;
    UINT gen = g_state.gen;   // keeps counting up: anything that cached the old cover by number must see a change
    g_state = NowPlaying::State();
    g_state.gen = gen + 1;
    g_trackKey.clear();
    g_appId.clear();
    g_propsOp.Reset();
    g_thumbOp.Reset();
    if (was) Notify(true);
}

void Poll() {
    if (g_failed) return;
    if (!g_statics) {
        HSTRING cls = nullptr;
        const wchar_t* name = L"Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager";
        if (FAILED(WindowsCreateString(name, (UINT32)wcslen(name), &cls))) { g_failed = true; return; }
        IInspectable* f = nullptr;
        HRESULT hr = RoGetActivationFactory(cls, IID_MgrStatics, reinterpret_cast<void**>(&f));
        WindowsDeleteString(cls);
        if (FAILED(hr) || !f) { g_failed = true; return; }   // older Windows: no media sessions
        g_statics.Attach(static_cast<IMgrStaticsAbi*>(f));
    }
    if (!g_mgr) {
        if (!g_mgrOp) { g_statics->RequestAsync(g_mgrOp.Put()); return; }
        ComPtr<IInspectable> r;
        int s = AsyncResult(g_mgrOp.Get(), r);
        if (s == 0) return;
        g_mgrOp.Reset();
        if (s < 0) return;
        g_mgr = As<IManagerAbi>(r.Get(), IID_Manager);
        if (!g_mgr) { g_failed = true; return; }
    }

    ComPtr<IInspectable> si;
    g_mgr->GetCurrentSession(si.Put());
    auto session = As<ISessionAbi>(si.Get(), IID_Session);
    if (!session) { Clear(); return; }

    HSTRING hid = nullptr;
    session->get_SourceAppUserModelId(&hid);
    std::wstring appId = Str(hid);
    bool appChanged = appId != g_appId;
    if (appChanged) {
        g_appId = appId;
        g_state.app = AppNameFor(appId);
        g_appIcon.Reset();
        g_appGen++;
        g_trackKey.clear();
        g_propsOp.Reset();
        g_thumbOp.Reset();
    }

    // Playing / paused
    ComPtr<IInspectable> pi;
    session->GetPlaybackInfo(pi.Put());
    INT32 status = 0;
    if (auto info = As<IPlaybackInfoAbi>(pi.Get(), IID_PlaybackInfo)) info->get_PlaybackStatus(&status);
    bool playing = status == 4;
    bool active = status == 4 || status == 5 || status == 2;

    // Where in the track
    ComPtr<IInspectable> ti;
    session->GetTimelineProperties(ti.Put());
    if (auto tl = As<ITimelineAbi>(ti.Get(), IID_Timeline)) {
        WinTimeSpan pos{}, start{}, end{};
        WinDateTime upd{};
        tl->get_Position(&pos);
        tl->get_StartTime(&start);
        tl->get_EndTime(&end);
        tl->get_LastUpdatedTime(&upd);
        g_posTicks = pos.ticks - start.ticks;
        g_startTicks = start.ticks;
        g_endTicks = end.ticks - start.ticks;
        g_updated = upd.ticks;
    }

    // Title, artist, artwork (asked for every couple of seconds; it's an async call)
    if (!g_propsOp && (appChanged || GetTickCount() - g_lastProps > 1500)) {
        session->TryGetMediaPropertiesAsync(g_propsOp.Put());
        g_lastProps = GetTickCount();
    }
    bool changed = playing != g_state.playing || active != g_state.active;
    bool layout = active != g_state.active;
    if (g_propsOp) {
        ComPtr<IInspectable> r;
        int s = AsyncResult(g_propsOp.Get(), r);
        if (s != 0) g_propsOp.Reset();
        if (s > 0) {
            if (auto mp = As<IMediaPropsAbi>(r.Get(), IID_MediaProps)) {
                HSTRING t = nullptr, a = nullptr, al = nullptr;
                mp->get_Title(&t);
                mp->get_Artist(&a);
                mp->get_AlbumTitle(&al);
                std::wstring title = Str(t), artist = Str(a), album = Str(al);
                std::wstring key = appId + L"\n" + title + L"\n" + artist + L"\n" + album;
                if (key != g_trackKey) {
                    g_trackKey = key;
                    g_state.title = title;
                    g_state.artist = artist;
                    g_state.album = album;
                    g_state.art.Reset();
                    g_state.tint = D2D1::ColorF(0, 0, 0, 0);
                    g_state.gen++;
                    g_thumbOp.Reset();
                    ComPtr<IInspectable> th;
                    mp->get_Thumbnail(th.Put());
                    if (auto ref = As<IStreamRefAbi>(th.Get(), IID_StreamRef)) ref->OpenReadAsync(g_thumbOp.Put());
                    changed = layout = true;
                }
            }
        }
    }
    if (g_thumbOp) {
        ComPtr<IInspectable> r;
        int s = AsyncResult(g_thumbOp.Get(), r);
        if (s != 0) g_thumbOp.Reset();
        if (s > 0) {
            g_state.art = DecodeArt(r.Get());
            if (g_state.art) g_state.tint = AverageColor(g_state.art.Get());
            g_state.gen++;
            changed = true;
        }
    }
    if (g_state.title.empty()) active = false;   // nothing to show yet
    if (active != g_state.active) layout = changed = true;
    g_state.active = active;
    g_state.playing = playing;
    g_state.duration = g_endTicks / 1e7;
    if (layout || changed) Notify(layout || changed);
    else if (g_state.active && g_state.playing)
        for (auto& d : g_app.docks) d->MediaChanged(false);   // only the dock's progress line moves
}

void Notify(bool layout) {
    for (auto& d : g_app.docks) d->MediaChanged(layout);
    if (layout) g_app.InvalidateAll();
    else if (g_state.active) g_app.InvalidateAll();
}

void CALLBACK Tick(HWND, UINT, UINT_PTR, DWORD) {
    Poll();
    // Faster while playing (the progress moves), slow when idle
    UINT want = g_state.active && g_state.playing ? 1000 : 2000;
    UINT& current = g_interval;
    if (want != current) { KillTimer(nullptr, g_timer); g_timer = SetTimer(nullptr, 0, want, Tick); current = want; }
}

// Fire-and-forget commands
void Command(int which, double seconds = 0) {
    if (!g_mgr) return;
    ComPtr<IInspectable> si;
    g_mgr->GetCurrentSession(si.Put());
    auto s = As<ISessionAbi>(si.Get(), IID_Session);
    if (!s) return;
    ComPtr<IInspectable> op;
    switch (which) {
    case 0: s->TryTogglePlayPauseAsync(op.Put()); break;
    case 1: s->TrySkipNextAsync(op.Put()); break;
    case 2: s->TrySkipPreviousAsync(op.Put()); break;
    case 3: s->TryChangePlaybackPositionAsync(g_startTicks + (INT64)(seconds * 1e7), op.Put()); break;
    }
    // Poll again shortly so the UI follows quickly
    SetTimer(nullptr, 0, 250, [](HWND, UINT, UINT_PTR id, DWORD) { KillTimer(nullptr, id); Poll(); });
}

} // namespace

// ===========================================================================
// The card
// ===========================================================================
namespace {

class Card {
public:
    // align: 0 centred under the anchor, -1 its left edge on the anchor's, 1 its right edge on the anchor's
    void Show(RECT anchor, bool above, UINT dpi, int align = 0) {
        anchor_ = anchor;
        above_ = above;
        if (drawer_) { drawer_ = false; rt_.Reset(); }
        sliding_ = closing_ = false;
        if (hwnd_) KillTimer(hwnd_, 3);   // (with no window yet this would stop someone else's thread timer)
        if (dpi != dpi_) { dpi_ = dpi; rt_.Reset(); }
        if (!Create()) return;
        int w = MulDiv(340, dpi_, 96), h = MulDiv(140, dpi_, 96), gap = MulDiv(8, dpi_, 96);
        MONITORINFO mi{sizeof mi};
        GetMonitorInfoW(MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST), &mi);
        int cx = (anchor.left + anchor.right) / 2;
        int want = align < 0 ? anchor.left : align > 0 ? anchor.right - w : cx - w / 2;
        int left = std::clamp(want, (int)mi.rcMonitor.left + gap, (int)mi.rcMonitor.right - w - gap);
        int top = above ? anchor.top - gap - h : anchor.bottom + gap;
        SetWindowPos(hwnd_, HWND_TOPMOST, left, top, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        outside_ = 0;
        SetTimer(hwnd_, 1, 80, nullptr);    // follow the mouse: hide when it leaves card and anchor
        SetTimer(hwnd_, 2, 500, nullptr);   // live progress
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    // The dock's drawer: a slim strip as tall as the dock that slides out from behind its side
    void ShowDrawer(RECT tile, RECT dock, HWND dockWnd, UINT dpi) {
        if (Visible() && drawer_ && !closing_) { anchor_ = tile; outside_ = 0; return; }
        anchor_ = tile;
        above_ = true;
        if (!drawer_) { drawer_ = true; rt_.Reset(); }
        if (dpi != dpi_) { dpi_ = dpi; rt_.Reset(); }
        if (!Create()) return;
        dock_ = dockWnd;
        int w = MulDiv(300, dpi_, 96), h = dock.bottom - dock.top, gap = MulDiv(6, dpi_, 96);
        MONITORINFO mi{sizeof mi};
        GetMonitorInfoW(MonitorFromRect(&dock, MONITOR_DEFAULTTONEAREST), &mi);
        bool right = dock.right + gap + w <= mi.rcMonitor.right - gap;   // out of the right side, or the left if there's no room
        toX_ = right ? dock.right + gap : dock.left - gap - w;
        fromX_ = right ? dock.right - w : dock.left;                     // starts hidden behind the dock
        y_ = dock.top;
        w_ = w;
        h_ = h;
        closing_ = false;
        sliding_ = true;
        slideStart_ = GetTickCount64();
        SetWindowPos(hwnd_, dock_ ? dock_ : HWND_TOPMOST, fromX_, y_, w_, h_, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        outside_ = 0;
        SetTimer(hwnd_, 1, 80, nullptr);
        SetTimer(hwnd_, 2, 500, nullptr);
        SetTimer(hwnd_, 3, 10, nullptr);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    void Slide() {
        float t = std::clamp((GetTickCount64() - slideStart_) / 190.0f, 0.0f, 1.0f);
        float e = 1 - (1 - t) * (1 - t) * (1 - t);   // ease out
        float k = closing_ ? 1 - e : e;
        int x = fromX_ + (int)std::lround((toX_ - fromX_) * k);
        SetWindowPos(hwnd_, dock_ && IsWindow(dock_) ? dock_ : HWND_TOPMOST, x, y_, w_, h_, SWP_NOACTIVATE | SWP_NOSIZE);
        if (t >= 1) {
            KillTimer(hwnd_, 3);
            sliding_ = false;
            if (closing_) { closing_ = false; HideNow(); }
        }
    }
    void Hide() {
        if (drawer_ && Visible() && !closing_) {   // slide back in behind the dock
            closing_ = true;
            sliding_ = true;
            slideStart_ = GetTickCount64();
            KillTimer(hwnd_, 1);
            SetTimer(hwnd_, 3, 10, nullptr);
            return;
        }
        if (!closing_) HideNow();
    }
    void HideNow() { if (hwnd_) { KillTimer(hwnd_, 1); KillTimer(hwnd_, 2); KillTimer(hwnd_, 3); ShowWindow(hwnd_, SW_HIDE); } hover_ = -1; closing_ = sliding_ = false; }
    bool Visible() const { return hwnd_ && IsWindowVisible(hwnd_); }
    bool HasMouse() const {
        if (!Visible() || closing_) return false;
        POINT p;
        GetCursorPos(&p);
        RECT r;
        GetWindowRect(hwnd_, &r);
        return PtInRect(&r, p) || PtInRect(&anchor_, p);
    }
    void Destroy() { closing_ = sliding_ = false; if (hwnd_) { SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0); DestroyWindow(hwnd_); hwnd_ = nullptr; rt_.Reset(); } }

private:
    enum { HPrev = 0, HPlay, HNext, HBar };

    bool Create() {
        if (hwnd_) return true;
        static bool reg = false;
        if (!reg) {
            WNDCLASSEXW wc{sizeof wc};
            wc.lpfnWndProc = Proc;
            wc.hInstance = g_app.hinst;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.lpszClassName = L"SkylineNowPlaying";
            RegisterClassExW(&wc);
            reg = true;
        }
        hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, L"SkylineNowPlaying", L"", WS_POPUP,
                                0, 0, 10, 10, nullptr, nullptr, g_app.hinst, this);
        if (!hwnd_) return false;
        const Theme& t = g_app.theme;
        BOOL dark = t.dark;
        DwmSetWindowAttribute(hwnd_, 20, &dark, sizeof dark);
        MARGINS m{-1, -1, -1, -1};
        DwmExtendFrameIntoClientArea(hwnd_, &m);
        DWORD corner = 2;
        DwmSetWindowAttribute(hwnd_, 33, &corner, sizeof corner);
        BOOL exclude = TRUE;
        DwmSetWindowAttribute(hwnd_, DWMWA_EXCLUDED_FROM_PEEK, &exclude, sizeof exclude);
        struct AP { int s, f; DWORD c; int a; };
        struct WD { int a; PVOID d; SIZE_T n; };
        using Fn = BOOL(WINAPI*)(HWND, WD*);
        static Fn fn = (Fn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowCompositionAttribute");
        (void)fn;
        ApplyGlassEffect(hwnd_, Part::Media);   // see-through: tint painted in Paint()
        return true;
    }

    // Where things go: the card (art left, text, progress, buttons below) or the drawer (one row, as tall as the dock)
    float H() const { RECT rc; GetClientRect(hwnd_, &rc); return rc.bottom * 96.0f / dpi_; }
    D2D1_RECT_F Art() const { float h = H(); return drawer_ ? D2D1::RectF(8, 8, h - 8, h - 8) : D2D1::RectF(16, 16, 108, 108); }
    float TextLeft() const { return drawer_ ? H() + 2 : 124; }
    float TextRight(float W) const { return drawer_ ? W - 14 - 3 * 32 - 6 : W - 16; }
    D2D1_RECT_F Button(int i, float W) const {
        if (drawer_) {
            float cx = W - 14 - (2 - i) * 32 - 16, cy = H() / 2 - 2;
            return D2D1::RectF(cx - 15, cy - 15, cx + 15, cy + 15);
        }
        float cx = 124 + (W - 124 - 16) / 2 + (i - 1) * 46;
        return D2D1::RectF(cx - 18, 100, cx + 18, 132);
    }
    D2D1_RECT_F Bar(float W) const {
        if (drawer_) { float h = H(); return D2D1::RectF(TextLeft(), h - 11, W - 14, h - 9); }
        return D2D1::RectF(124, 80, W - 16, 84);
    }

    void Paint() {
        if (!rt_) {
            RECT rc;
            GetClientRect(hwnd_, &rc);
            auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                                      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                                                      (float)dpi_, (float)dpi_);
            if (FAILED(g_gfx.d2d->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(rc.right, rc.bottom)),
                                                         rt_.Put())))
                return;
            rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            rt_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1), brush_.Put());
            auto fmt = [](const wchar_t* fam, float size, DWRITE_FONT_WEIGHT w, DWRITE_TEXT_ALIGNMENT al) {
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
            };
            fTitle_ = fmt(L"Segoe UI", 15, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING);
            fArtist_ = fmt(L"Segoe UI", 13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING);
            fSmall_ = fmt(L"Segoe UI", 11, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING);
            fSmallR_ = fmt(L"Segoe UI", 11, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING);
            std::wstring icons = L"Segoe MDL2 Assets";
            ComPtr<IDWriteFontCollection> fc;
            if (SUCCEEDED(g_gfx.dwrite->GetSystemFontCollection(fc.Put(), FALSE)) && fc) {
                UINT32 idx;
                BOOL ex = FALSE;
                if (SUCCEEDED(fc->FindFamilyName(L"Segoe Fluent Icons", &idx, &ex)) && ex) icons = L"Segoe Fluent Icons";
            }
            fGlyph_ = fmt(icons.c_str(), 16, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
            fGlyphBig_ = fmt(icons.c_str(), 20, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
            artBmp_.Reset();
            appBmp_.Reset();
        }
        const Theme& th = g_app.theme;
        const auto& s = NowPlaying::Current();
        D2D1_SIZE_F sz = rt_->GetSize();
        float W = sz.width;
        rt_->BeginDraw();
        rt_->Clear(D2D1::ColorF(0, 0, 0, 0));
        if (D2D1_COLOR_F tint = GlassTint(Part::Media); tint.a > 0) {
            brush_->SetColor(tint);
            rt_->FillRectangle(D2D1::RectF(0, 0, W, sz.height), brush_.Get());
        }
        PaintVibe(rt_.Get(), D2D1::RectF(0, 0, W, sz.height), Part::Media);
        PaintLiquid(rt_.Get(), D2D1::RectF(0, 0, W, sz.height), 8);
        // A wash of the cover's colour, like the Mac
        if (g_app.settings.npTint && s.tint.a > 0) {
            D2D1_COLOR_F c = s.tint;
            c.a = th.dark ? 0.30f : 0.22f;
            brush_->SetColor(c);
            rt_->FillRectangle(D2D1::RectF(0, 0, W, sz.height), brush_.Get());
        }
        // artwork
        D2D1_RECT_F ar = Art();
        if (artGen_ != s.gen) { artBmp_.Reset(); artGen_ = s.gen; }
        if (!artBmp_ && s.art) rt_->CreateBitmapFromWicBitmap(s.art.Get(), nullptr, artBmp_.Put());
        ComPtr<ID2D1RoundedRectangleGeometry> clipGeo;
        g_gfx.d2d->CreateRoundedRectangleGeometry(D2D1::RoundedRect(ar, 8, 8), clipGeo.Put());
        if (artBmp_ && clipGeo) {
            ComPtr<ID2D1Layer> layer;
            rt_->CreateLayer(layer.Put());
            rt_->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clipGeo.Get()), layer.Get());
            rt_->DrawBitmap(artBmp_.Get(), ar, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            rt_->PopLayer();
        } else {
            brush_->SetColor(D2D1::ColorF(th.fg.r, th.fg.g, th.fg.b, 0.08f));
            rt_->FillRoundedRectangle(D2D1::RoundedRect(ar, 8, 8), brush_.Get());
            wchar_t g = 0xE8D6;   // music note
            brush_->SetColor(th.fgDim);
            rt_->DrawTextW(&g, 1, fGlyphBig_.Get(), ar, brush_.Get());
        }
        // text
        auto text = [&](const std::wstring& t, IDWriteTextFormat* f, D2D1_RECT_F r, D2D1_COLOR_F c) {
            if (t.empty() || !f) return;
            brush_->SetColor(c);
            rt_->DrawTextW(t.c_str(), (UINT32)t.size(), f, r, brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        };
        float tl = TextLeft(), tr = TextRight(W);
        if (drawer_) {   // title and artist stacked in the middle of the strip
            float my = H() / 2 - 2;
            text(s.title, fTitle_.Get(), D2D1::RectF(tl, my - 19, tr, my + 1), th.fg);
            text(s.artist.empty() ? s.album : s.artist, fSmall_.Get(), D2D1::RectF(tl, my + 1, tr, my + 18), th.fgDim);
        } else {
            text(s.title, fTitle_.Get(), D2D1::RectF(tl, 14, tr, 36), th.fg);
            text(s.artist.empty() ? s.album : s.artist, fArtist_.Get(), D2D1::RectF(tl, 36, tr, 56), th.fgDim);
        }
        // the app it's playing in
        float ax = 124;
        int ipx = MulDiv(14, dpi_, 96);
        if (appGen_ != g_appGen) { appBmp_.Reset(); appGen_ = g_appGen; }   // another app is playing now
        if (!drawer_) {
            if (!appBmp_)
                if (auto img = NowPlaying::AppIconFor(ipx)) rt_->CreateBitmapFromWicBitmap(img.Get(), nullptr, appBmp_.Put());
            if (appBmp_) { DrawImageHQ(rt_.Get(), appBmp_.Get(), D2D1::RectF(ax, 60, ax + 14, 74)); ax += 19; }
            text(s.app, fSmall_.Get(), D2D1::RectF(ax, 58, W - 16, 76), th.fgDim);
        }
        // progress
        D2D1_RECT_F bar = Bar(W);
        float frac = s.duration > 0 ? (float)std::clamp(s.position / s.duration, 0.0, 1.0) : 0;
        brush_->SetColor(D2D1::ColorF(th.fg.r, th.fg.g, th.fg.b, 0.18f));
        rt_->FillRoundedRectangle(D2D1::RoundedRect(bar, 2, 2), brush_.Get());
        brush_->SetColor(hover_ == HBar ? th.accent : D2D1::ColorF(th.fg.r, th.fg.g, th.fg.b, 0.85f));
        rt_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bar.left, bar.top, bar.left + (bar.right - bar.left) * frac, bar.bottom), 2, 2), brush_.Get());
        if (s.duration > 0 && !drawer_) {
            auto mmss = [](double sec) {
                int t = (int)std::max(0.0, sec);
                wchar_t b[16];
                swprintf(b, 16, L"%d:%02d", t / 60, t % 60);
                return std::wstring(b);
            };
            text(mmss(s.position), fSmall_.Get(), D2D1::RectF(bar.left, 86, bar.left + 60, 98), th.fgDim);
            text(mmss(s.duration), fSmallR_.Get(), D2D1::RectF(bar.right - 60, 86, bar.right, 98), th.fgDim);
        }
        // controls
        const wchar_t glyphs[3] = {0xE892, (wchar_t)(s.playing ? 0xE769 : 0xE768), 0xE893};
        for (int i = 0; i < 3; ++i) {
            D2D1_RECT_F r = Button(i, W);
            if (hover_ == i) {
                brush_->SetColor(th.hover);
                rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F((r.left + r.right) / 2, (r.top + r.bottom) / 2), 16, 16), brush_.Get());
            }
            brush_->SetColor(th.fg);
            rt_->DrawTextW(&glyphs[i], 1, i == 1 ? fGlyphBig_.Get() : fGlyph_.Get(), r, brush_.Get());
        }
        static int fails = 0;
        if (FAILED(rt_->EndDraw())) { rt_.Reset(); artBmp_.Reset(); appBmp_.Reset(); if (++fails < 3) InvalidateRect(hwnd_, nullptr, FALSE); } else fails = 0;
    }

    int HitAt(float x, float y, float W) const {
        for (int i = 0; i < 3; ++i) {
            D2D1_RECT_F r = Button(i, W);
            if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return i;
        }
        D2D1_RECT_F b = Bar(W);
        if (x >= b.left - 2 && x < b.right + 2 && y >= b.top - 7 && y < b.bottom + 7) return HBar;
        return -1;
    }

    static LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR) reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        auto* self = reinterpret_cast<Card*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (!self) return DefWindowProcW(h, msg, wp, lp);
        if (!self->hwnd_) self->hwnd_ = h;
        RECT rc;
        GetClientRect(h, &rc);
        float W = rc.right * 96.0f / self->dpi_;
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
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_TIMER:
            if (wp == 1) {
                if (!NowPlaying::Current().active) { self->Hide(); return 0; }
                if (self->HasMouse()) self->outside_ = 0;
                else if (++self->outside_ >= 4) self->Hide();   // ~300 ms away from both
            } else if (wp == 2) {
                InvalidateRect(h, nullptr, FALSE);
            } else if (wp == 3) {
                self->Slide();
            }
            return 0;
        case WM_MOUSEMOVE: {
            int hv = self->HitAt(x, y, W);
            if (hv != self->hover_) { self->hover_ = hv; InvalidateRect(h, nullptr, FALSE); }
            return 0;
        }
        case WM_LBUTTONUP: {
            int hv = self->HitAt(x, y, W);
            const auto& s = NowPlaying::Current();
            if (hv == HPrev) NowPlaying::Previous();
            else if (hv == HPlay) NowPlaying::TogglePlay();
            else if (hv == HNext) NowPlaying::Next();
            else if (hv == HBar && s.duration > 0) {
                D2D1_RECT_F b = self->Bar(W);
                NowPlaying::Seek(std::clamp((x - b.left) / (b.right - b.left), 0.0f, 1.0f) * s.duration);
            }
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        }
        return DefWindowProcW(h, msg, wp, lp);
    }

    HWND hwnd_ = nullptr;
    UINT dpi_ = 96;
    RECT anchor_{};
    bool above_ = true;
    bool drawer_ = false, sliding_ = false, closing_ = false;   // the dock's drawer and its slide
    HWND dock_ = nullptr;
    int fromX_ = 0, toX_ = 0, y_ = 0, w_ = 0, h_ = 0;
    ULONGLONG slideStart_ = 0;
    int hover_ = -1, outside_ = 0;
    UINT artGen_ = 0, appGen_ = 0;
    ComPtr<ID2D1HwndRenderTarget> rt_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<ID2D1Bitmap> artBmp_, appBmp_;
    ComPtr<IDWriteTextFormat> fTitle_, fArtist_, fSmall_, fSmallR_, fGlyph_, fGlyphBig_;
};

Card g_card;

} // namespace

// ===========================================================================
namespace NowPlaying {

const State& Current() {
    // live position between the app's own (infrequent) reports
    if (g_state.active) {
        INT64 pos = g_posTicks;
        if (g_state.playing && g_updated > 0) {
            FILETIME ft;
            GetSystemTimeAsFileTime(&ft);
            INT64 now = ((INT64)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
            if (now > g_updated) pos += now - g_updated;
        }
        if (g_endTicks > 0) pos = std::min(pos, g_endTicks);
        g_state.position = pos / 1e7;
    }
    return g_state;
}

void ApplySettings() {
    bool on = g_app.settings.npEnabled;
    if (on && !g_timer) {
        g_failed = false;
        g_timer = SetTimer(nullptr, 0, 2000, Tick);
        g_interval = 2000;
        Poll();
    } else if (!on && g_timer) {
        KillTimer(nullptr, g_timer);
        g_timer = 0;
        g_card.Hide();
        Clear();
    }
    if (on) g_card.Destroy();   // pick up theme/backdrop changes
}

bool InDock() { return g_app.settings.npEnabled && g_app.settings.npWhere != 1 && g_state.active; }
bool InBar() { return g_app.settings.npEnabled && g_app.settings.npWhere != 0 && g_state.active; }
bool BarAtClock() { return g_app.settings.npBarPos == 1; }
bool BarCentered() { return g_app.settings.npBarPos == 2; }
bool BarAfterMenus() { return g_app.settings.npBarPos == 3; }

void TogglePlay() { Command(0); }
void Next() { Command(1); }
void Previous() { Command(2); }
void Seek(double s) { Command(3, s); }

void ShowCard(RECT anchor, bool above, UINT dpi, int align) { if (g_state.active) g_card.Show(anchor, above, dpi, align); }
void ShowDrawer(RECT tile, RECT dock, HWND dockWnd, UINT dpi) { if (g_state.active) g_card.ShowDrawer(tile, dock, dockWnd, dpi); }
void HideCard() { g_card.Hide(); }
void Restyle() { g_card.Destroy(); }
bool CardHasMouse() { return g_card.HasMouse(); }

void Shutdown() {
    if (g_timer) { KillTimer(nullptr, g_timer); g_timer = 0; }
    g_card.Destroy();
    g_propsOp.Reset();
    g_thumbOp.Reset();
    g_mgrOp.Reset();
    g_mgr.Reset();
    g_statics.Reset();
    UINT gen = g_state.gen;
    g_state = State();
    g_state.gen = gen + 1;
}

ComPtr<IWICBitmapSource> AppIconFor(int px) {
    if (g_appIcon && g_appIconPx == px) return g_appIcon;
    g_appIconPx = px;
    g_appIcon.Reset();
    std::wstring l = ToLower(g_appId);
    if (l.empty()) return g_appIcon;
    for (const auto& a : AllApps(0xFFFFFFFF)) {
        if (a.setting) continue;
        bool match = (a.key.size() >= l.size() && a.key.compare(a.key.size() - l.size(), l.size(), l) == 0) ||
                     (!a.exe.empty() && ToLower(FileNameOf(a.exe)) == l);
        if (match) { g_appIcon = LoadAppImage(a.direct ? a.exe : a.target, px); break; }
    }
    if (!g_appIcon && l.find(L'!') != std::wstring::npos) g_appIcon = LoadAppImage(L"shell:AppsFolder\\" + g_appId, px);
    return g_appIcon;
}

} // namespace NowPlaying
