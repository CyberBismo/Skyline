// Skyline — Copyright © 2026 Yen-Zen & Cyber. All rights reserved. See LICENSE.txt.
// Enhanced Tiling — a second tiling engine, separate from tiling.cpp.
//   Round 1: a layout tree per screen (splits and windows), LIVE resizing while an edge is dragged,
//   hotkeys for size / equal size / focus / swap, and per-app rules ([Tiling] Rules).
// Event driven like the plain tiler: nothing runs until a window opens, closes, moves or is resized.
#include "enhancedtiling.h"
#include "tiling.h"
#include "version.h"
#include "app.h"
#include "bar.h"
#include "dock.h"
#include <shellscalingapi.h>
#include <map>
#include <set>
#include <mutex>
#include <atomic>
#include <thread>

namespace {

constexpr UINT_PTR kTimerEnh = 0x7112;
constexpr UINT_PTR kTimerAnim = 0x7113;    // window glide (only runs while something moves)
constexpr UINT_PTR kTimerFocus = 0x7114;   // focus follows mouse (only runs when switched on)
constexpr ULONGLONG kAnimMs = 140;

enum Hk {
    HkFocusL = 0x5200, HkFocusR, HkFocusU, HkFocusD,
    HkSwapL, HkSwapR, HkSwapU, HkSwapD,
    HkGrowL, HkGrowR, HkGrowU, HkGrowD,
    HkEqual, HkFloat, HkRetile,
    HkMaster,
    HkSpace1, HkSpace2, HkSpace3, HkSpace4, HkSpace5, HkSpace6, HkSpace7, HkSpace8,
    HkSend1, HkSend2, HkSend3, HkSend4, HkSend5, HkSend6, HkSend7, HkSend8,
    HkOverview,
    HkLast = HkOverview
};
int g_nSpaces = 4;           // workspaces per screen (2 to 8; "+" and "x" in Mission Control)
#define kSpaces g_nSpaces

bool Cloaked(HWND h) {
    BOOL c = FALSE;
    DwmGetWindowAttribute(h, DWMWA_CLOAKED, &c, sizeof c);
    return c != FALSE;
}

RECT FrameBounds(HWND h) {
    RECT r{};
    if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof r))) GetWindowRect(h, &r);
    return r;
}

bool Near(const RECT& a, const RECT& b) {
    return abs(a.left - b.left) <= 2 && abs(a.top - b.top) <= 2 && abs(a.right - b.right) <= 2 && abs(a.bottom - b.bottom) <= 2;
}

POINT Center(const RECT& r) { return {(r.left + r.right) / 2, (r.top + r.bottom) / 2}; }

std::wstring Trim(const std::wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && iswspace(s[a])) ++a;
    while (b > a && iswspace(s[b - 1])) --b;
    return s.substr(a, b - a);
}

HWND g_moving = nullptr;   // window the user is dragging/resizing right now

// One node in the layout tree: a window (leaf) or a split of two parts.
struct Node {
    bool leaf = true;
    HWND win = nullptr;
    bool horiz = true;               // split left | right (true) or top / bottom (false)
    float ratio = 0.5f;              // share of the first part
    Node* parent = nullptr;
    std::unique_ptr<Node> a, b;
    RECT area{};                     // what this node was given at the last layout
    RECT rect{};                     // leaf: where the window goes
    LONG mid = 0, firstEdge = 0, secondEdge = 0;   // divider
};

struct Tree { std::unique_ptr<Node> root; };

struct Rule { std::wstring exe; int kind; int n; };   // kind: 0 float, 1 monitor n, 2 space n (round 3)

void Leaves(Node* n, std::vector<Node*>& out) {
    if (!n) return;
    if (n->leaf) { out.push_back(n); return; }
    Leaves(n->a.get(), out);
    Leaves(n->b.get(), out);
}

Node* FindLeaf(Node* n, HWND h) {
    if (!n) return nullptr;
    if (n->leaf) return n->win == h ? n : nullptr;
    if (Node* r = FindLeaf(n->a.get(), h)) return r;
    return FindLeaf(n->b.get(), h);
}

int CountLeaves(const Node* n) {
    if (!n) return 0;
    return n->leaf ? 1 : CountLeaves(n->a.get()) + CountLeaves(n->b.get());
}

void Equalize(Node* n) {
    if (!n || n->leaf) return;
    int ca = CountLeaves(n->a.get()), cb = CountLeaves(n->b.get());
    n->ratio = (ca + cb) > 0 ? (float)ca / (float)(ca + cb) : 0.5f;
    Equalize(n->a.get());
    Equalize(n->b.get());
}

// The leaf goes away; its sibling takes the place of their parent.
void RemoveLeaf(Tree& t, Node* leaf) {
    if (!leaf) return;
    if (!leaf->parent) { t.root.reset(); return; }
    Node* p = leaf->parent;
    std::unique_ptr<Node> sib = (p->a.get() == leaf) ? std::move(p->b) : std::move(p->a);
    p->a.reset();
    p->b.reset();   // the leaf is gone now
    p->leaf = sib->leaf;
    p->win = sib->win;
    p->horiz = sib->horiz;
    p->ratio = sib->ratio;
    p->a = std::move(sib->a);
    p->b = std::move(sib->b);
    if (p->a) p->a->parent = p;
    if (p->b) p->b->parent = p;
}

// The leaf splits in two: the window that was there stays first, the new one comes second.
void SplitLeafAt(Node* target, HWND win, bool wide, bool newFirst) {
    auto oldLeaf = std::make_unique<Node>();
    oldLeaf->win = target->win;
    oldLeaf->parent = target;
    auto newLeaf = std::make_unique<Node>();
    newLeaf->win = win;
    newLeaf->parent = target;
    target->leaf = false;
    target->win = nullptr;
    target->horiz = wide;
    target->ratio = 0.5f;
    if (newFirst) { target->a = std::move(newLeaf); target->b = std::move(oldLeaf); }
    else { target->a = std::move(oldLeaf); target->b = std::move(newLeaf); }
}

void SplitLeaf(Node* target, HWND win, const RECT& fallback) {
    RECT base = IsRectEmpty(&target->rect) ? fallback : target->rect;
    SplitLeafAt(target, win, (base.right - base.left) >= (base.bottom - base.top), false);
}

// Pictures of windows (max 480 px wide), taken in the BACKGROUND now and then while Enhanced Tiling runs, so that
// a window that is hidden (another workspace) still has a recent picture for the dock preview and the overview.
// Windows can't draw a live picture of a hidden window, and taking one at the moment of switching could stall
// Skyline for a hung app, so the switch itself never waits for a picture.
struct Raw { int w = 0, h = 0; std::vector<DWORD> px; };   // top-down 32-bit pixels, opaque

struct SnapStore {
    std::mutex mu;
    std::map<HWND, Raw> raw;
    std::map<HWND, ULONGLONG> when;
    std::atomic<bool> busy{false};
};
SnapStore g_snap;

bool CaptureRaw(HWND h, Raw& out) {   // GDI only: safe on a worker thread
    bool iconic = IsIconic(h) != FALSE;   // a minimized window is drawn at its normal size
    if (!IsWindowVisible(h) || IsHungAppWindow(h)) return false;
    RECT r;
    if (iconic) {
        WINDOWPLACEMENT wp{sizeof wp};
        if (!GetWindowPlacement(h, &wp)) return false;
        r = wp.rcNormalPosition;
    } else if (!GetWindowRect(h, &r)) return false;
    int w = r.right - r.left, hh = r.bottom - r.top;
    if (w < 50 || hh < 50 || w > 8000 || hh > 8000) return false;
    bool ok = false;
    HDC sdc = GetDC(nullptr);
    HDC mdc = CreateCompatibleDC(sdc);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -hh;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bmp) {
        HGDIOBJ old = SelectObject(mdc, bmp);
        if (PrintWindow(h, mdc, 0x2 /* PW_RENDERFULLCONTENT */)) {
            int sw = std::min(w, 480), sh = std::max(1, hh * sw / w);
            BITMAPINFO b2 = bi;
            b2.bmiHeader.biWidth = sw;
            b2.bmiHeader.biHeight = -sh;
            void* bits2 = nullptr;
            HBITMAP smallBmp = CreateDIBSection(sdc, &b2, DIB_RGB_COLORS, &bits2, nullptr, 0);
            HDC d2 = CreateCompatibleDC(sdc);
            if (smallBmp && d2) {
                HGDIOBJ old2 = SelectObject(d2, smallBmp);
                SetStretchBltMode(d2, HALFTONE);
                SetBrushOrgEx(d2, 0, 0, nullptr);
                StretchBlt(d2, 0, 0, sw, sh, mdc, 0, 0, w, hh, SRCCOPY);
                GdiFlush();
                out.w = sw;
                out.h = sh;
                out.px.assign((DWORD*)bits2, (DWORD*)bits2 + (size_t)sw * sh);
                for (auto& p : out.px) p |= 0xFF000000;   // opaque (GDI leaves alpha empty)
                ok = true;
                if (iconic) {   // an empty picture (nothing but one colour) is no use: keep the old one
                    bool same = true;
                    for (auto p : out.px) if (p != out.px[0]) { same = false; break; }
                    if (same) ok = false;
                }
                SelectObject(d2, old2);
            }
            if (d2) DeleteDC(d2);
            if (smallBmp) DeleteObject(smallBmp);
        }
        SelectObject(mdc, old);
        DeleteObject(bmp);
    }
    DeleteDC(mdc);
    ReleaseDC(nullptr, sdc);
    return ok;
}

// Starts one low-priority worker for the windows whose picture is older than 12 s (never two at a time).
void KickCapture(std::vector<HWND> wins, bool force = false) {
    ULONGLONG now = GetTickCount64();
    {
        std::lock_guard<std::mutex> lk(g_snap.mu);
        for (auto it = g_snap.raw.begin(); it != g_snap.raw.end();) it = IsWindow(it->first) ? std::next(it) : g_snap.raw.erase(it);   // closed windows
        if (!force)
            wins.erase(std::remove_if(wins.begin(), wins.end(), [&](HWND h) {
                auto it = g_snap.when.find(h);
                return it != g_snap.when.end() && now - it->second < 12000;
            }), wins.end());
    }
    if (wins.empty() || g_snap.busy.exchange(true)) return;
    std::thread([wins]() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        for (HWND h : wins) {
            Raw r;
            bool ok = CaptureRaw(h, r);
            if (!ok && IsIconic(h)) continue;   // minimized and nothing usable: leave the earlier picture alone
            std::lock_guard<std::mutex> lk(g_snap.mu);
            g_snap.when[h] = GetTickCount64();   // (also after a failure: don't try again at once)
            if (ok) g_snap.raw[h] = std::move(r);
        }
        g_snap.busy = false;
    }).detach();
}

ComPtr<IWICBitmapSource> RawToWic(HWND h) {
    ComPtr<IWICBitmapSource> out;
    if (!g_gfx.wic) return out;
    std::lock_guard<std::mutex> lk(g_snap.mu);
    auto it = g_snap.raw.find(h);
    if (it == g_snap.raw.end() || it->second.px.empty()) return out;
    ComPtr<IWICBitmap> wb;
    if (SUCCEEDED(g_gfx.wic->CreateBitmapFromMemory(it->second.w, it->second.h, GUID_WICPixelFormat32bppPBGRA, it->second.w * 4,
                                                    (UINT)(it->second.px.size() * 4), (BYTE*)it->second.px.data(), wb.Put()))) {
        IWICBitmapSource* src = wb.Get();
        src->AddRef();
        out.Attach(src);
    }
    return out;
}

class ETiler {
public:
    void Start();
    void Stop();
    bool Running() const { return running_; }
    bool Manages(HWND h) const { return running_ && targets_.count(h) != 0; }
    bool IsFloating(HWND h) const { return floating_.count(h) != 0; }
    void FlushPending() { if (pendingRetile_ && running_) { pendingRetile_ = false; Schedule(300); } }
    void Schedule(UINT ms = 60);
    void OnTimer();
    void OnAnimTimer();
    void OnFocusTimer();
    void StartAnim(HWND h, const RECT& from, const RECT& to, int l, int t, int r, int b);
    void Retile();
    void ApplySettings();
    bool OnHotkey(int id);
    void ToggleFloat(HWND h);
    void Shutdown() { Stop(); if (hotkeys_) UnregisterHotkeys(); }
    static LRESULT CALLBACK DivProc(HWND w, UINT m, WPARAM wp, LPARAM lp);   // a divider window

private:
    static void CALLBACK OnEvent(HWINEVENTHOOK, DWORD ev, HWND h, LONG obj, LONG child, DWORD, DWORD);
    bool Manageable(HWND h) const;
    RECT WorkArea(HMONITOR mon) const;
    int GapFor(HMONITOR mon) const;
    void LayoutNode(Node* n, const RECT& area, int g) const;
    void Place(HWND h, const RECT& target);
    void Relayout(HMONITOR mon, HWND skip);
    bool InTree(HWND h, HMONITOR* mon = nullptr, Node** leaf = nullptr);
    bool ApplyResize(HWND h, const RECT& now);
    void LiveResize(HWND h);
    void SwapAt(HWND moved, POINT pt);
    bool HitTarget(HWND moved, POINT pt, HWND* tgt, int* zone, RECT* preview) const;
    void DropAt(HWND moved, POINT pt);
    void UpdateSnap(HWND h);
    void ShowPreview(const RECT& r);
    void HidePreview();
    void UpdateDividers();
    void HideDividers();
    void DestroyDividers();
    bool IsLiveNode(Node* n);
    bool MonOfNode(Node* n, HMONITOR* mon);
    void DragDivider(Node* n, POINT pt);
    HWND Neighbor(HWND from, int dx, int dy) const;
    // overview of all workspaces (popup)
    void PaintOverview(HWND w);
    int CardAt(LPARAM lp) const;
    RECT CardRect(int i) const;
    void HideOverview() { if (spacesWnd_) ShowWindow(spacesWnd_, SW_HIDE); }
    // workspaces
    int CurSpace(HMONITOR m) const { auto it = cur_.find(m); return it == cur_.end() ? 1 : it->second; }
    void SwitchSpace(HMONITOR mon, int n);
    bool ParkWindow(HWND h, HMONITOR mon, int n);   // hide it in workspace n of that screen
    void SendToSpace(HWND h, int n);
    void ApplySpaceRule(HWND h);
    void ShowAllHidden();
    void PromoteMaster(HWND h);
    void PersistHidden();                 // the list of hidden windows is kept on disk (crash recovery)
    void RecoverHidden();                 // after a crash: show them again
    void ShowSpaceToast(HMONITOR mon);    // a short "1 2 3 4" popup with the current workspace lit
    void RebuildDocks();
public:
    void ToggleOverview(HMONITOR mon);
    bool LastSpaceFree() const {
        for (auto& [m, c] : cur_) if (c == kSpaces) return false;
        for (auto& [k, t] : parked_) if (k.second == kSpaces && t.root) return false;
        return true;
    }
    void DropLastSpace();
    void ReRegisterHotkeys() { if (hotkeys_) { UnregisterHotkeys(); RegisterHotkeys(); } }
    std::vector<EnhancedSpaceInfo> Info(HMONITOR mon, SIZE* waSize);
    void GoToSpace(HMONITOR mon, int n) { SwitchSpace(mon, n); }
    static LRESULT CALLBACK OverviewProc(HWND w, UINT m, WPARAM wp, LPARAM lp);
    int CurrentSpace(HMONITOR m) const { return CurSpace(m); }
    bool IsHiddenWin(HWND h) const { return hidden_.count(h) != 0; }
    ComPtr<IWICBitmapSource> Snapshot(HWND h) const { return RawToWic(h); }
    bool Reveal(HWND h);                  // go to the workspace the window sits in and give it the focus
private:
    void LoadRules();
    void ApplyMonitorRule(HWND h);
    void SyncLocationHooks();
    void RegisterHotkeys();
    void UnregisterHotkeys();

    bool running_ = false;
    bool hotkeys_ = false;
    bool retiling_ = false;
    bool pendingRetile_ = false;
    ULONGLONG lastLive_ = 0;
    SIZE dragSize_{};                 // size of the window when the drag began
    bool dragResized_ = false;        // this drag changed the size (so it is a resize, never a swap)
    std::vector<HWINEVENTHOOK> hooks_;
    std::map<DWORD, HWINEVENTHOOK> locHooks_;
    std::map<HMONITOR, Tree> trees_;
    std::set<HWND> floating_, known_, wasZoomed_, ruled_;
    mutable std::map<HWND, std::wstring> exeCache_;
    std::map<HWND, RECT> placed_, targets_, seen_;
    std::map<HWND, int> tries_;
    std::vector<Rule> rules_;
    // workspaces: the shown one lives in trees_; the others are parked here with their windows hidden (SW_HIDE)
    std::map<std::pair<HMONITOR, int>, Tree> parked_;
    std::map<HMONITOR, int> cur_;             // shown workspace per screen (1-4)
    std::set<HWND> hidden_;                   // windows we hid
    std::map<HWND, ULONGLONG> pendingShow_;   // windows just shown again: counted as present until Windows catches up
    HWND focusAfter_ = nullptr;
    bool recovered_ = false;
    HWND toast_ = nullptr;
    HWND spacesWnd_ = nullptr;        // the overview popup
    HMONITOR spacesMon_ = nullptr;
    int spacesHover_ = -1;
    RECT spacesWa_{};                 // work area of that screen (what the little screens are scaled from)
    int cardW_ = 0, cardH_ = 0, bodyW_ = 0, bodyH_ = 0;
    bool useDividers_ = true, useSnap_ = true, animate_ = true, focusFollows_ = false, noAnim_ = false;
    struct Anim { RECT from, to; int l, t, r, b; ULONGLONG t0; };
    std::map<HWND, Anim> anims_;
    POINT lastCur_{-1, -1};
    std::vector<HWND> divs_;          // divider windows (thin, nearly invisible, in the gaps)
    HWND divDrag_ = nullptr;          // the divider being dragged
    bool divHover_ = false;
    ULONGLONG lastDiv_ = 0, lastSnap_ = 0;
    HWND preview_ = nullptr;          // drop preview
    RECT lastPreview_{};
};

std::unique_ptr<ETiler> g_et;

// ---------------------------------------------------------------------------
void ETiler::Start() {
    if (running_) return;
    running_ = true;
    auto hook = [&](DWORD a, DWORD b) {
        HWINEVENTHOOK h = SetWinEventHook(a, b, nullptr, OnEvent, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        if (h) hooks_.push_back(h);
    };
    hook(EVENT_SYSTEM_MOVESIZESTART, EVENT_SYSTEM_MOVESIZEEND);
    hook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND);
    hook(EVENT_OBJECT_DESTROY, EVENT_OBJECT_HIDE);
    hook(EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED);
    // LOCATIONCHANGE is hooked per process (only apps we tile), see SyncLocationHooks().
    known_.clear();
    ruled_.clear();
    Retile();
}

void ETiler::Stop() {
    if (!running_) return;
    running_ = false;
    for (auto h : hooks_) UnhookWinEvent(h);
    hooks_.clear();
    for (auto& [pid, h] : locHooks_) UnhookWinEvent(h);
    locHooks_.clear();
    if (g_app.ctlWnd) { KillTimer(g_app.ctlWnd, kTimerEnh); KillTimer(g_app.ctlWnd, kTimerAnim); KillTimer(g_app.ctlWnd, kTimerFocus); }
    anims_.clear();
    g_moving = nullptr;
    divDrag_ = nullptr;
    DestroyDividers();
    HidePreview();
    ShowAllHidden();   // every workspace window comes back (nothing stays hidden when tiling stops)
    if (toast_) { DestroyWindow(toast_); toast_ = nullptr; }
    if (spacesWnd_) { DestroyWindow(spacesWnd_); spacesWnd_ = nullptr; }
    trees_.clear();
    placed_.clear();
    targets_.clear();
    tries_.clear();
    seen_.clear();
    // Windows stay where they are.
}

void ETiler::ApplySettings() {
    bool want = g_app.settings.tilingEnabled && g_app.settings.enhancedTiling;
    bool hk = want && g_app.settings.tilingMode >= 1;
    if (hk && !hotkeys_) RegisterHotkeys();
    else if (!hk && hotkeys_) UnregisterHotkeys();
    if (!recovered_) { recovered_ = true; RecoverHidden(); }
    LoadRules();
    useDividers_ = GetPrivateProfileIntW(L"Tiling", L"Dividers", 1, g_app.settings.path.c_str()) != 0;
    g_nSpaces = std::clamp((int)GetPrivateProfileIntW(L"Tiling", L"Spaces", 4, g_app.settings.path.c_str()), 2, 8);
    useSnap_ = GetPrivateProfileIntW(L"Tiling", L"Snap", 1, g_app.settings.path.c_str()) != 0;
    animate_ = GetPrivateProfileIntW(L"Tiling", L"Animate", 1, g_app.settings.path.c_str()) != 0;
    focusFollows_ = GetPrivateProfileIntW(L"Tiling", L"FocusFollowsMouse", 0, g_app.settings.path.c_str()) != 0;
    if (!animate_) { anims_.clear(); if (g_app.ctlWnd) KillTimer(g_app.ctlWnd, kTimerAnim); }
    if (g_app.ctlWnd) {
        if (want && focusFollows_) SetTimer(g_app.ctlWnd, kTimerFocus, 120, nullptr);
        else KillTimer(g_app.ctlWnd, kTimerFocus);
    }
    if (want) {
        if (!running_) Start();
        else { placed_.clear(); Retile(); }
    } else {
        Stop();
    }
}

void ETiler::Schedule(UINT ms) {
    if (running_ && g_app.ctlWnd) SetTimer(g_app.ctlWnd, kTimerEnh, ms, nullptr);
}

// Windows glide to their new place (~140 ms, ease-out). One 16 ms timer, alive only while something moves.
void ETiler::StartAnim(HWND h, const RECT& from, const RECT& to, int l, int t, int r, int b) {
    anims_[h] = Anim{from, to, l, t, r, b, GetTickCount64()};
    if (g_app.ctlWnd) SetTimer(g_app.ctlWnd, kTimerAnim, 16, nullptr);
}

void ETiler::OnAnimTimer() {
    ULONGLONG now = GetTickCount64();
    for (auto it = anims_.begin(); it != anims_.end();) {
        HWND h = it->first;
        Anim& a = it->second;
        if (!running_ || !IsWindow(h) || !IsWindowVisible(h) || IsIconic(h) || h == g_moving) { it = anims_.erase(it); continue; }
        float k = std::min(1.0f, (float)(now - a.t0) / (float)kAnimMs);
        float e = 1.0f - (1.0f - k) * (1.0f - k) * (1.0f - k);
        auto lerp = [&](LONG x, LONG y) { return (int)std::lround(x + (y - x) * e); };
        RECT c{lerp(a.from.left, a.to.left), lerp(a.from.top, a.to.top), lerp(a.from.right, a.to.right), lerp(a.from.bottom, a.to.bottom)};
        SetWindowPos(h, nullptr, c.left - a.l, c.top - a.t, (c.right - c.left) + a.l + a.r, (c.bottom - c.top) + a.t + a.b,
                     SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
        if (k >= 1.0f) { seen_[h] = a.to; tries_[h] = 0; it = anims_.erase(it); }
        else ++it;
    }
    if (anims_.empty() && g_app.ctlWnd) { KillTimer(g_app.ctlWnd, kTimerAnim); Schedule(120); }   // one last check that everyone landed
}

// Focus follows the mouse: only between tiled windows, never while a button is down or something else (a menu, Spotlight) has focus.
void ETiler::OnFocusTimer() {
    if (!running_ || !focusFollows_) return;
    POINT p;
    if (!GetCursorPos(&p) || (p.x == lastCur_.x && p.y == lastCur_.y)) return;
    lastCur_ = p;
    if (g_moving || divDrag_ || GameMode()) return;
    if ((GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON)) & 0x8000) return;
    HWND fg = GetForegroundWindow();
    if (!fg || !targets_.count(fg)) return;
    HWND w = WindowFromPoint(p);
    if (!w) return;
    HWND root = GetAncestor(w, GA_ROOT);
    if (!root || root == fg || !targets_.count(root) || !IsWindowVisible(root) || IsIconic(root)) return;
    SetForegroundWindow(root);
}

void ETiler::OnTimer() {
    KillTimer(g_app.ctlWnd, kTimerEnh);
    Retile();
}

// Rules: [Tiling] Rules=firefox.exe=float, code.exe=monitor 2, spotify.exe=space 3
void ETiler::LoadRules() {
    rules_.clear();
    wchar_t buf[2048] = {};
    GetPrivateProfileStringW(L"Tiling", L"Rules", L"", buf, 2048, g_app.settings.path.c_str());
    for (auto& item : SplitList(ToLower(buf), L',')) {
        size_t eq = item.find(L'=');
        if (eq == std::wstring::npos) continue;
        Rule r{Trim(item.substr(0, eq)), -1, 0};
        std::wstring act = Trim(item.substr(eq + 1));
        if (r.exe.empty()) continue;
        if (act == L"float") r.kind = 0;
        else if (act.rfind(L"monitor", 0) == 0) { r.kind = 1; r.n = _wtoi(Trim(act.substr(7)).c_str()); }
        else if (act.rfind(L"space", 0) == 0) { r.kind = 2; r.n = _wtoi(Trim(act.substr(5)).c_str()); }
        if (r.kind >= 0) rules_.push_back(r);
    }
}

void ETiler::ApplyMonitorRule(HWND h) {
    auto it = exeCache_.find(h);
    if (it == exeCache_.end()) return;
    for (auto& r : rules_) {
        if (r.kind != 1 || r.exe != it->second || r.n < 1) continue;
        std::vector<HMONITOR> mons;
        EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR m, HDC, LPRECT, LPARAM lp) -> BOOL { ((std::vector<HMONITOR>*)lp)->push_back(m); return TRUE; }, (LPARAM)&mons);
        if (r.n > (int)mons.size()) return;
        HMONITOR want = mons[r.n - 1];
        if (MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST) == want) return;
        MONITORINFO mi{sizeof mi};
        GetMonitorInfoW(want, &mi);
        SetWindowPos(h, nullptr, mi.rcWork.left + 60, mi.rcWork.top + 60, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        return;
    }
}

// ---------------------------------------------------------------------------
bool ETiler::InTree(HWND h, HMONITOR* mon, Node** leaf) {
    for (auto& [m, t] : trees_)
        if (Node* n = FindLeaf(t.root.get(), h)) {
            if (mon) *mon = m;
            if (leaf) *leaf = n;
            return true;
        }
    return false;
}

void CALLBACK ETiler::OnEvent(HWINEVENTHOOK, DWORD ev, HWND h, LONG obj, LONG child, DWORD, DWORD) {
    ETiler* t = g_et.get();
    if (!t || !t->running_ || obj != OBJID_WINDOW || child != CHILDID_SELF || !h) return;
    if (GameMode()) { t->pendingRetile_ = true; return; }
    bool automatic = g_app.settings.tilingMode != 2;

    switch (ev) {
    case EVENT_SYSTEM_MOVESIZESTART: {
        g_moving = h;
        RECT fb0 = FrameBounds(h);
        t->dragSize_ = {fb0.right - fb0.left, fb0.bottom - fb0.top};
        t->dragResized_ = false;
        return;
    }
    case EVENT_SYSTEM_MOVESIZEEND: {
        g_moving = nullptr;
        t->HidePreview();
        POINT pt;
        GetCursorPos(&pt);
        if (t->InTree(h)) {
            RECT fb = FrameBounds(h);
            auto tg = t->targets_.find(h);
            bool resized = t->dragResized_ || (tg != t->targets_.end() &&
                           (std::abs((fb.right - fb.left) - (tg->second.right - tg->second.left)) > 6 ||
                            std::abs((fb.bottom - fb.top) - (tg->second.bottom - tg->second.top)) > 6));
            if (resized) {
                t->ApplyResize(h, fb);
                t->placed_.clear();
                t->Schedule(30);
                return;
            }
            t->DropAt(h, pt);
        }
        if (automatic) t->Schedule(30);
        return;
    }
    case EVENT_OBJECT_LOCATIONCHANGE:
        if (g_moving) {
            if (h == g_moving) {
                t->LiveResize(h);   // live: neighbours follow while the edge is dragged
                if (!t->dragResized_) t->UpdateSnap(h);   // dragging the whole window: show where it would land
            }
            return;
        }
        if (!automatic) return;
        if (t->InTree(h) || (t->known_.count(h) && !IsZoomed(h))) t->Schedule(120);
        else if (t->useDividers_ && GetAncestor(h, GA_ROOT) == h) {   // another app maximized / restored: dividers must (not) cover it
            static std::set<HWND> zoomed;
            bool z = IsZoomed(h) != FALSE;
            if (z != (zoomed.count(h) != 0)) { if (z) zoomed.insert(h); else zoomed.erase(h); t->UpdateDividers(); }
        }
        return;
    default: {
        if (!automatic) return;
        bool ours = t->InTree(h);
        if (ev == EVENT_OBJECT_SHOW || ev == EVENT_OBJECT_UNCLOAKED || ev == EVENT_SYSTEM_MINIMIZEEND) {
            if (GetAncestor(h, GA_ROOT) != h) return;
            if (!ours && !t->Manageable(h)) return;
        } else if (!ours) {
            return;
        }
        t->Schedule(ev == EVENT_OBJECT_SHOW ? 120 : 60);
        return;
    }
    }
}

// ---------------------------------------------------------------------------
bool ETiler::Manageable(HWND h) const {
    if (!IsWindow(h) || !IsWindowVisible(h) || IsIconic(h) || Cloaked(h)) return false;
    if (floating_.count(h) || hidden_.count(h)) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId()) return false;

    LONG style = GetWindowLongW(h, GWL_STYLE);
    LONG ex = GetWindowLongW(h, GWL_EXSTYLE);
    bool appWindow = (ex & WS_EX_APPWINDOW) != 0;
    if ((ex & WS_EX_TOOLWINDOW) && !appWindow) return false;
    if ((ex & WS_EX_NOACTIVATE) || (ex & WS_EX_TOPMOST)) return false;
    if (GetWindow(h, GW_OWNER) && !appWindow) return false;
    if (!(style & WS_THICKFRAME)) return false;
    if (GetWindowTextLengthW(h) == 0) return false;

    wchar_t cls[96] = {};
    GetClassNameW(h, cls, 96);
    static const wchar_t* skipClasses[] = {L"#32770", L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd",
                                           L"Windows.UI.Core.CoreWindow", L"XamlExplorerHostIslandWindow",
                                           L"TopLevelWindowForOverflowXamlIsland", L"ForegroundStaging"};
    for (auto c : skipClasses) if (!lstrcmpW(cls, c)) return false;

    auto cached = exeCache_.find(h);
    if (cached == exeCache_.end()) cached = exeCache_.emplace(h, ToLower(FileNameOf(ProcessImagePath(pid)))).first;
    const std::wstring& exe = cached->second;
    static const wchar_t* skipExe[] = {L"startmenuexperiencehost.exe", L"searchhost.exe", L"shellexperiencehost.exe",
                                       L"shellhost.exe", L"lockapp.exe", L"textinputhost.exe"};
    for (auto e : skipExe) if (exe == e) return false;
    for (auto& f : g_app.settings.tilingFloat) if (exe == f) return false;
    for (auto& r : rules_) if (r.kind == 0 && r.exe == exe) return false;   // rule: exe=float

    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
    RECT r;
    GetWindowRect(h, &r);
    auto placedHere = targets_.find(h);
    bool inOurSlot = placedHere != targets_.end() && Near(FrameBounds(h), placedHere->second);
    if (!IsZoomed(h) && !inOurSlot && (style & WS_CAPTION) != WS_CAPTION && r.left <= mi.rcMonitor.left &&
        r.top <= mi.rcMonitor.top && r.right >= mi.rcMonitor.right && r.bottom >= mi.rcMonitor.bottom)
        return false;

    if (IsZoomed(h) && known_.count(h)) return false;
    return true;
}

RECT ETiler::WorkArea(HMONITOR mon) const {
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(mon, &mi);
    RECT a = mi.rcWork;
    a.top = mi.rcMonitor.top;
    for (auto& b : g_app.bars)
        if (b->Monitor() == mon) {
            int reserved = b->ReservedBottom();
            if (reserved != 0) a.top = std::max<LONG>(a.top, reserved);
        }
    Dock* dock = g_app.DockOn(mon);
    if (g_app.settings.tilingDockSpace && dock) {
        int top = dock->TopEdge(mon);
        if (top > a.top && top < a.bottom) a.bottom = top;
    }
    return a;
}

int ETiler::GapFor(HMONITOR mon) const {
    UINT dx = 96, dy = 96;
    if (FAILED(GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy))) dx = 96;
    return MulDiv(g_app.settings.tilingGap, dx, 96);
}

void ETiler::LayoutNode(Node* n, const RECT& area, int g) const {
    n->area = area;
    if (n->leaf) { n->rect = area; return; }
    int h2 = g / 2;
    n->ratio = std::clamp(n->ratio, 0.1f, 0.9f);
    if (n->horiz) {
        int mid = area.left + (int)((area.right - area.left) * n->ratio);
        RECT a{area.left, area.top, mid - h2, area.bottom};
        RECT b{mid + (g - h2), area.top, area.right, area.bottom};
        n->mid = mid; n->firstEdge = mid - h2; n->secondEdge = mid + (g - h2);
        LayoutNode(n->a.get(), a, g);
        LayoutNode(n->b.get(), b, g);
    } else {
        int mid = area.top + (int)((area.bottom - area.top) * n->ratio);
        RECT a{area.left, area.top, area.right, mid - h2};
        RECT b{area.left, mid + (g - h2), area.right, area.bottom};
        n->mid = mid; n->firstEdge = mid - h2; n->secondEdge = mid + (g - h2);
        LayoutNode(n->a.get(), a, g);
        LayoutNode(n->b.get(), b, g);
    }
}

void ETiler::Place(HWND h, const RECT& target) {
    if (IsZoomed(h)) {
        ShowWindowAsync(h, SW_RESTORE);   // async: a busy app mustn't freeze Skyline
        Schedule(150);
        return;
    }
    if (auto an = anims_.find(h); an != anims_.end()) {   // on its way: just change where it ends
        if (!Near(an->second.to, target)) {
            RECT cur = FrameBounds(h);
            an->second.from = cur; an->second.to = target; an->second.t0 = GetTickCount64();
        }
        placed_[h] = target;
        return;
    }
    RECT fb = FrameBounds(h);
    {
        auto seen = seen_.find(h);
        if (seen != seen_.end() && !Near(seen->second, fb)) tries_[h] = 0;
        seen_[h] = fb;
    }
    auto it = placed_.find(h);
    if (it != placed_.end() && Near(it->second, target)) {
        if (Near(fb, target)) { tries_[h] = 0; return; }
        if (tries_[h] >= 2) return;   // the app refuses (minimum size): stop pushing, no jitter
        ++tries_[h];
    } else {
        tries_[h] = 0;
    }
    RECT wr;
    GetWindowRect(h, &wr);
    int l = fb.left - wr.left, t = fb.top - wr.top, r = wr.right - fb.right, b = wr.bottom - fb.bottom;
    if (animate_ && !noAnim_ && it != placed_.end() && !Near(it->second, target) && !IsIconic(h) && !GameMode()) {
        StartAnim(h, fb, target, l, t, r, b);
        placed_[h] = target;
        return;
    }
    SetWindowPos(h, nullptr, target.left - l, target.top - t, (target.right - target.left) + l + r,
                 (target.bottom - target.top) + t + b,
                 SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
    placed_[h] = target;
}

// Lay out one screen again from its tree and place everything (except `skip`, which the user is holding).
void ETiler::Relayout(HMONITOR mon, HWND skip) {
    auto it = trees_.find(mon);
    if (it == trees_.end() || !it->second.root) return;
    int g = GapFor(mon);
    RECT wa = WorkArea(mon);
    RECT area{wa.left + g, wa.top + g, wa.right - g, wa.bottom - g};
    LayoutNode(it->second.root.get(), area, g);
    std::vector<Node*> leaves;
    Leaves(it->second.root.get(), leaves);
    for (Node* l : leaves) {
        if (l->win != skip) Place(l->win, l->rect);
        targets_[l->win] = l->rect;
        known_.insert(l->win);
    }
    UpdateDividers();
}

void ETiler::Retile() {
    if (g_moving && !IsWindow(g_moving)) g_moving = nullptr;
    if (!running_ || retiling_ || g_moving) return;
    if (GameMode()) { pendingRetile_ = true; HideDividers(); return; }
    retiling_ = true;

    struct Ctx { const ETiler* self; std::vector<HWND> wins; } ctx{this, {}};
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(lp);
        if (c->self->Manageable(h)) c->wins.push_back(h);
        return TRUE;
    }, (LPARAM)&ctx);
    std::vector<HWND>& now = ctx.wins;

    for (HWND h : known_) if (IsWindow(h) && IsZoomed(h)) wasZoomed_.insert(h);
    for (HWND h : now)
        if (wasZoomed_.count(h) && !IsZoomed(h)) { placed_.erase(h); wasZoomed_.erase(h); }
    for (auto it = wasZoomed_.begin(); it != wasZoomed_.end();) it = IsWindow(*it) ? std::next(it) : wasZoomed_.erase(it);

    // windows we just showed again (another workspace): count them as present until Windows has caught up
    {
        ULONGLONG tick = GetTickCount64();
        for (auto it = pendingShow_.begin(); it != pendingShow_.end();) {
            HWND h = it->first;
            if (!IsWindow(h) || IsWindowVisible(h) || tick - it->second > 1500) { it = pendingShow_.erase(it); continue; }
            if (!IsIconic(h) && std::find(now.begin(), now.end(), h) == now.end()) now.push_back(h);
            ++it;
        }
    }
    // rules: a new window can be sent to a given screen and / or workspace
    for (HWND h : now)
        if (!ruled_.count(h)) { ruled_.insert(h); ApplyMonitorRule(h); ApplySpaceRule(h); }
    now.erase(std::remove_if(now.begin(), now.end(), [&](HWND h) { return hidden_.count(h) != 0; }), now.end());
    // hidden windows that were closed: out of their workspace
    for (auto it = hidden_.begin(); it != hidden_.end();) it = IsWindow(*it) ? std::next(it) : hidden_.erase(it);
    {   // pictures of windows that are gone
        std::lock_guard<std::mutex> lk(g_snap.mu);
        for (auto it = g_snap.raw.begin(); it != g_snap.raw.end();) it = IsWindow(it->first) ? std::next(it) : g_snap.raw.erase(it);
        for (auto it = g_snap.when.begin(); it != g_snap.when.end();) it = IsWindow(it->first) ? std::next(it) : g_snap.when.erase(it);
    }
    for (auto it = parked_.begin(); it != parked_.end();) {
        std::vector<Node*> lv;
        Leaves(it->second.root.get(), lv);
        for (Node* l : lv) if (!IsWindow(l->win)) RemoveLeaf(it->second, FindLeaf(it->second.root.get(), l->win));
        it = it->second.root ? std::next(it) : parked_.erase(it);
    }

    // forget windows that are gone
    auto prune = [](auto& m) { for (auto it = m.begin(); it != m.end();) it = IsWindow(it->first) ? std::next(it) : m.erase(it); };
    prune(placed_); prune(exeCache_); prune(tries_); prune(seen_);
    for (auto it = floating_.begin(); it != floating_.end();) it = IsWindow(*it) ? std::next(it) : floating_.erase(it);
    for (auto it = known_.begin(); it != known_.end();) it = IsWindow(*it) ? std::next(it) : known_.erase(it);
    for (auto it = ruled_.begin(); it != ruled_.end();) it = IsWindow(*it) ? std::next(it) : ruled_.erase(it);
    targets_.clear();

    std::map<HMONITOR, std::vector<HWND>> perMon;
    for (HWND h : now) perMon[MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST)].push_back(h);

    // take out windows that are gone, minimized, floating, or on another screen now
    for (auto it = trees_.begin(); it != trees_.end();) {
        auto& list = perMon[it->first];
        std::vector<Node*> leaves;
        Leaves(it->second.root.get(), leaves);
        std::vector<HWND> gone;
        for (Node* l : leaves)
            if (std::find(list.begin(), list.end(), l->win) == list.end()) gone.push_back(l->win);
        for (HWND w : gone) RemoveLeaf(it->second, FindLeaf(it->second.root.get(), w));
        it = it->second.root ? std::next(it) : trees_.erase(it);
    }

    // new windows join (oldest first): they split the focused window, or else the last one
    HWND fg = GetForegroundWindow();
    for (auto& [mon, list] : perMon) {
        Tree& tree = trees_[mon];
        RECT wa = WorkArea(mon);
        for (auto it = list.rbegin(); it != list.rend(); ++it) {
            HWND h = *it;
            if (FindLeaf(tree.root.get(), h)) continue;
            if (!tree.root) {
                tree.root = std::make_unique<Node>();
                tree.root->win = h;
                continue;
            }
            Node* target = FindLeaf(tree.root.get(), fg);
            if (!target || fg == h) {
                target = tree.root.get();
                while (!target->leaf) target = target->b.get();
            }
            SplitLeaf(target, h, wa);
        }
        if (!tree.root) trees_.erase(mon);
    }

    for (auto& [mon, tree] : trees_) Relayout(mon, nullptr);
    SyncLocationHooks();
    UpdateDividers();
    retiling_ = false;
    if (!trees_.empty()) KeepBarsOnTop();
    if (!trees_.empty()) RecheckFullscreen();
    if (hotkeys_) {   // pictures for hidden-window previews / the overview, in the background
        std::vector<HWND> vis;
        for (auto& [m, t] : trees_) { std::vector<Node*> lv; Leaves(t.root.get(), lv); for (Node* l : lv) vis.push_back(l->win); }
        KickCapture(vis);
    }
    if (focusAfter_) {   // after a workspace switch: a window of the new workspace gets the focus
        HWND f = focusAfter_;
        focusAfter_ = nullptr;
        if (IsWindow(f) && IsWindowVisible(f)) SetForegroundWindow(f);
    }
}

// ---------------------------------------------------------------------------
// Workspaces: four per screen. The one on show lives in trees_; the others are parked with their windows
// hidden (SW_HIDE). Everything is shown again when tiling stops.
void ETiler::ShowAllHidden() {
    for (HWND h : hidden_) if (IsWindow(h)) ShowWindowAsync(h, SW_SHOWNA);
    hidden_.clear();
    parked_.clear();
    pendingShow_.clear();
    cur_.clear();
    PersistHidden();
    RebuildDocks();
    if (toast_) ShowWindow(toast_, SW_HIDE);
}

bool ETiler::ParkWindow(HWND h, HMONITOR mon, int n) {
    if (n < 1 || n > kSpaces) return false;
    Tree& pt = parked_[{mon, n}];
    if (!pt.root) {
        pt.root = std::make_unique<Node>();
        pt.root->win = h;
    } else {
        Node* target = pt.root.get();
        while (!target->leaf) target = target->b.get();
        SplitLeaf(target, h, WorkArea(mon));
    }
    hidden_.insert(h);
    ShowWindowAsync(h, SW_HIDE);   // async: an app that doesn't answer must not freeze Skyline
    targets_.erase(h);
    placed_.erase(h);
    PersistHidden();
    RebuildDocks();
    return true;
}

void ETiler::SwitchSpace(HMONITOR mon, int n) {
    if (n < 1 || n > kSpaces || n == CurSpace(mon)) return;
    int old = CurSpace(mon);
    auto it = trees_.find(mon);
    if (it != trees_.end() && it->second.root) {
        std::vector<Node*> lv;
        Leaves(it->second.root.get(), lv);
        for (Node* l : lv) {
            hidden_.insert(l->win);
            ShowWindowAsync(l->win, SW_HIDE);
            targets_.erase(l->win);
            placed_.erase(l->win);
        }
        parked_[{mon, old}] = std::move(it->second);
        trees_.erase(it);
    }
    cur_[mon] = n;
    auto pk = parked_.find({mon, n});
    if (pk != parked_.end()) {
        Tree tr = std::move(pk->second);
        parked_.erase(pk);
        std::vector<Node*> lv;
        Leaves(tr.root.get(), lv);
        ULONGLONG tick = GetTickCount64();
        for (Node* l : lv) {
            if (!IsWindow(l->win)) continue;
            hidden_.erase(l->win);
            pendingShow_[l->win] = tick;
            ShowWindowAsync(l->win, SW_SHOWNA);
            placed_.erase(l->win);
            if (!focusAfter_) focusAfter_ = l->win;
        }
        trees_[mon] = std::move(tr);
    }
    PersistHidden();
    RebuildDocks();
    ShowSpaceToast(mon);
    g_app.InvalidateAll();   // the dots in the top bar
    Schedule(30);
}

// The last workspace goes away: anyone standing on it moves to the one before, and its windows follow.
void ETiler::DropLastSpace() {
    const int last = kSpaces, to = last - 1;
    std::vector<HMONITOR> onLast;
    for (auto& [m, c] : cur_) if (c == last) onLast.push_back(m);
    for (HMONITOR m : onLast) SwitchSpace(m, to);
    std::vector<std::pair<HMONITOR, std::vector<HWND>>> moves;
    for (auto it = parked_.begin(); it != parked_.end();) {
        if (it->first.second != last) { ++it; continue; }
        std::vector<HWND> ws;
        if (it->second.root) {
            std::vector<Node*> lv;
            Leaves(it->second.root.get(), lv);
            for (Node* l : lv) if (IsWindow(l->win)) ws.push_back(l->win);
        }
        moves.push_back({it->first.first, std::move(ws)});
        it = parked_.erase(it);
    }
    ULONGLONG tick = GetTickCount64();
    for (auto& [m, ws] : moves)
        for (HWND w : ws) {
            if (CurSpace(m) == to) {   // that one is on show: the window comes back and Retile tiles it in
                hidden_.erase(w);
                pendingShow_[w] = tick;
                ShowWindowAsync(w, SW_SHOWNA);
                placed_.erase(w);
            } else {
                ParkWindow(w, m, to);
            }
        }
    PersistHidden();
    RebuildDocks();
    Schedule(30);
}

void ETiler::SendToSpace(HWND h, int n) {
    HMONITOR mon = nullptr;
    Node* leaf = nullptr;
    if (!InTree(h, &mon, &leaf) || n < 1 || n > kSpaces || n == CurSpace(mon)) return;
    RemoveLeaf(trees_[mon], leaf);
    ParkWindow(h, mon, n);
    // another window of this screen gets the focus
    auto t = trees_.find(mon);
    if (t != trees_.end() && t->second.root) {
        std::vector<Node*> lv;
        Leaves(t->second.root.get(), lv);
        if (!lv.empty()) focusAfter_ = lv[0]->win;
    }
    ShowSpaceToast(mon);
    g_app.InvalidateAll();
    Schedule(30);
}

void ETiler::ApplySpaceRule(HWND h) {
    auto it = exeCache_.find(h);
    if (it == exeCache_.end()) return;
    for (auto& r : rules_) {
        if (r.kind != 2 || r.exe != it->second) continue;
        HMONITOR mon = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
        if (r.n >= 1 && r.n <= kSpaces && r.n != CurSpace(mon)) ParkWindow(h, mon, r.n);
        return;
    }
}

// ---------------------------------------------------------------------------
// Hidden windows are written to skyline-hidden.txt next to skyline.ini (window handle + process id) whenever they
// change, so that after a crash they can be shown again at the next start.
static std::wstring HiddenFilePath() {
    std::wstring path = g_app.settings.path;
    size_t slash = path.find_last_of(L"\\/");
    return (slash == std::wstring::npos ? L"" : path.substr(0, slash + 1)) + L"skyline-hidden.txt";
}

void ETiler::PersistHidden() {
    std::wstring path = HiddenFilePath();
    if (hidden_.empty()) { DeleteFileW(path.c_str()); return; }
    std::string out;
    for (HWND h : hidden_) {
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        out += std::to_string((unsigned long long)(ULONG_PTR)h) + " " + std::to_string((unsigned long)pid) + "\n";
    }
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) { DWORD wr; WriteFile(f, out.data(), (DWORD)out.size(), &wr, nullptr); CloseHandle(f); }
}

void ETiler::RecoverHidden() {
    std::wstring path = HiddenFilePath();
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    char buf[8192] = {};
    DWORD rd = 0;
    ReadFile(f, buf, sizeof buf - 1, &rd, nullptr);
    CloseHandle(f);
    DeleteFileW(path.c_str());
    char* p = buf;
    while (*p) {
        char* end = nullptr;
        unsigned long long hv = strtoull(p, &end, 10);
        if (end == p) break;
        unsigned long pid = strtoul(end, &p, 10);
        HWND h = (HWND)(ULONG_PTR)hv;
        DWORD now = 0;
        if (IsWindow(h) && !IsWindowVisible(h) && GetWindowThreadProcessId(h, &now) && now == pid)
            ShowWindowAsync(h, SW_SHOWNA);   // (the same process still owns that handle: it is the window we hid)
        while (*p == '\n' || *p == '\r' || *p == ' ') ++p;
    }
}

void ETiler::RebuildDocks() {
    for (auto& d : g_app.docks) d->RebuildSoon();   // hidden windows stay in the dock; the list is made again
}

bool ETiler::Reveal(HWND h) {
    for (auto& [key, tree] : parked_) {
        if (!FindLeaf(tree.root.get(), h)) continue;
        HMONITOR mon = key.first;
        int n = key.second;
        SwitchSpace(mon, n);
        focusAfter_ = h;
        return true;
    }
    return false;
}

static LRESULT CALLBACK ToastProc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(w, &ps);
        RECT rc;
        GetClientRect(w, &rc);
        LONG_PTR v = GetWindowLongPtrW(w, GWLP_USERDATA);
        int cur = (int)(v & 0xFF), mask = (int)((v >> 8) & 0xFF);
        HBRUSH bg = CreateSolidBrush(RGB(32, 34, 40));
        FillRect(dc, &rc, bg);
        DeleteObject(bg);
        SetBkMode(dc, TRANSPARENT);
        HFONT font = CreateFontW(-22, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        HGDIOBJ oldFont = SelectObject(dc, font);
        int cell = (rc.right - rc.left) / kSpaces;
        for (int i = 0; i < kSpaces; ++i) {
            RECT c{rc.left + i * cell, rc.top, rc.left + (i + 1) * cell, rc.bottom};
            if (i + 1 == cur) {
                HBRUSH ab = CreateSolidBrush(RGB(60, 120, 230));
                HGDIOBJ ob = SelectObject(dc, ab), op = SelectObject(dc, GetStockObject(NULL_PEN));
                int cx = (c.left + c.right) / 2, cy = (c.top + c.bottom) / 2, d = 38;
                Ellipse(dc, cx - d / 2, cy - d / 2, cx + d / 2 + 1, cy + d / 2 + 1);
                SelectObject(dc, op);
                SelectObject(dc, ob);
                DeleteObject(ab);
                SetTextColor(dc, RGB(255, 255, 255));
            } else {
                SetTextColor(dc, ((mask >> i) & 1) ? RGB(215, 220, 230) : RGB(100, 104, 115));   // bright = has windows
            }
            wchar_t t[2] = {(wchar_t)(L'1' + i), 0};
            DrawTextW(dc, t, 1, &c, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        SelectObject(dc, oldFont);
        DeleteObject(font);
        EndPaint(w, &ps);
        return 0;
    }
    case WM_TIMER:
        KillTimer(w, 1);
        ShowWindow(w, SW_HIDE);
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    }
    return DefWindowProcW(w, m, wp, lp);
}

void ETiler::ShowSpaceToast(HMONITOR mon) {
    HINSTANCE hi = GetModuleHandleW(nullptr);
    const int cell = 56, W = cell * kSpaces, H = 64;
    if (!toast_) {
        WNDCLASSEXW wc{sizeof wc};
        wc.lpfnWndProc = ToastProc;
        wc.hInstance = hi;
        wc.lpszClassName = L"SkylineSpaceToast";
        RegisterClassExW(&wc);   // (a second registration just fails, harmlessly)
        toast_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_TOPMOST,
                                 L"SkylineSpaceToast", L"", WS_POPUP, 0, 0, W, H, nullptr, nullptr, hi, nullptr);
        if (!toast_) return;
        SetLayeredWindowAttributes(toast_, 0, 235, LWA_ALPHA);
        SetWindowRgn(toast_, CreateRoundRectRgn(0, 0, W + 1, H + 1, 28, 28), FALSE);
    }
    int mask = 0;
    for (int sp = 1; sp <= kSpaces; ++sp) {
        bool occ;
        if (sp == CurSpace(mon)) { auto t = trees_.find(mon); occ = t != trees_.end() && t->second.root; }
        else { auto p = parked_.find({mon, sp}); occ = p != parked_.end() && p->second.root; }
        if (occ) mask |= 1 << (sp - 1);
    }
    SetWindowLongPtrW(toast_, GWLP_USERDATA, (LONG_PTR)(CurSpace(mon) | (mask << 8)));
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(mon, &mi);
    int x = (mi.rcWork.left + mi.rcWork.right - W) / 2, y = mi.rcWork.bottom - H - 120;
    SetWindowPos(toast_, HWND_TOPMOST, x, y, W, H, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(toast_, nullptr, FALSE);
    SetTimer(toast_, 1, 1000, nullptr);   // gone after a second
}

// ---------------------------------------------------------------------------
// Overview of all workspaces on one screen: four smallBmp screens side by side with the windows in them
// (their last picture when there is one). Click one to go there; Esc or a click elsewhere closes it.
RECT ETiler::CardRect(int i) const {
    const int pad = 16, gap = 14;
    int x = pad + i * (cardW_ + gap);
    return {x, pad, x + cardW_, pad + cardH_};
}

int ETiler::CardAt(LPARAM lp) const {
    POINT p{(short)LOWORD(lp), (short)HIWORD(lp)};
    for (int i = 0; i < kSpaces; ++i) { RECT r = CardRect(i); if (PtInRect(&r, p)) return i; }
    return -1;
}

void ETiler::ToggleOverview(HMONITOR mon) {
    if (!running_) return;
    if (spacesWnd_ && IsWindowVisible(spacesWnd_)) { HideOverview(); return; }
    HINSTANCE hi = GetModuleHandleW(nullptr);
    static bool reg = false;
    if (!reg) {
        WNDCLASSEXW wc{sizeof wc};
        wc.lpfnWndProc = OverviewProc;
        wc.hInstance = hi;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"SkylineSpaces";
        RegisterClassExW(&wc);
        reg = true;
    }
    spacesMon_ = mon;
    spacesWa_ = WorkArea(mon);
    int waW = std::max<LONG>(1, spacesWa_.right - spacesWa_.left), waH = std::max<LONG>(1, spacesWa_.bottom - spacesWa_.top);
    cardW_ = 240;
    bodyW_ = cardW_ - 20;
    bodyH_ = std::clamp(bodyW_ * waH / waW, 70, 170);
    cardH_ = 32 + bodyH_ + 10;
    int W = 16 * 2 + kSpaces * cardW_ + (kSpaces - 1) * 14, H = 16 * 2 + cardH_;
    if (!spacesWnd_) {
        spacesWnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"SkylineSpaces", L"", WS_POPUP, 0, 0, W, H, nullptr, nullptr, hi, nullptr);
        if (!spacesWnd_) return;
    }
    spacesHover_ = -1;
    int x = (spacesWa_.left + spacesWa_.right - W) / 2, y = spacesWa_.top + 12;
    SetWindowPos(spacesWnd_, HWND_TOPMOST, x, y, W, H, SWP_SHOWWINDOW);
    SetWindowRgn(spacesWnd_, CreateRoundRectRgn(0, 0, W + 1, H + 1, 26, 26), TRUE);
    InvalidateRect(spacesWnd_, nullptr, FALSE);
    SetForegroundWindow(spacesWnd_);
    SetFocus(spacesWnd_);
}

void ETiler::PaintOverview(HWND w) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(w, &ps);
    RECT rc;
    GetClientRect(w, &rc);
    HDC mdc = CreateCompatibleDC(dc);
    HBITMAP bm = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    HGDIOBJ oldBm = SelectObject(mdc, bm);
    HBRUSH bg = CreateSolidBrush(RGB(28, 30, 36));
    FillRect(mdc, &rc, bg);
    DeleteObject(bg);
    SetBkMode(mdc, TRANSPARENT);
    HFONT big = CreateFontW(-15, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HFONT smallBmp = CreateFontW(-11, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HGDIOBJ oldFont = SelectObject(mdc, big);
    HGDIOBJ oldPen = SelectObject(mdc, GetStockObject(NULL_PEN));
    HGDIOBJ oldBrush = SelectObject(mdc, GetStockObject(NULL_BRUSH));
    int waW = std::max<LONG>(1, spacesWa_.right - spacesWa_.left), waH = std::max<LONG>(1, spacesWa_.bottom - spacesWa_.top);

    for (int i = 0; i < kSpaces; ++i) {
        int n = i + 1;
        RECT c = CardRect(i);
        bool cur = n == CurSpace(spacesMon_);
        // the windows of this workspace
        Tree* tree = nullptr;
        if (cur) { auto t = trees_.find(spacesMon_); if (t != trees_.end()) tree = &t->second; }
        else { auto p = parked_.find({spacesMon_, n}); if (p != parked_.end()) tree = &p->second; }
        std::vector<Node*> lv;
        if (tree && tree->root) Leaves(tree->root.get(), lv);

        HBRUSH cb = CreateSolidBrush(i == spacesHover_ ? RGB(54, 58, 70) : RGB(40, 43, 52));
        SelectObject(mdc, cb);
        RoundRect(mdc, c.left, c.top, c.right, c.bottom, 16, 16);
        SelectObject(mdc, GetStockObject(NULL_BRUSH));
        DeleteObject(cb);
        if (cur) {
            HPEN ap = CreatePen(PS_SOLID, 2, RGB(60, 120, 230));
            SelectObject(mdc, ap);
            RoundRect(mdc, c.left + 1, c.top + 1, c.right - 1, c.bottom - 1, 16, 16);
            SelectObject(mdc, GetStockObject(NULL_PEN));
            DeleteObject(ap);
        }
        // header: "1   3 windows"
        wchar_t head[64];
        swprintf(head, 64, L"%d", n);
        RECT hr{c.left + 12, c.top + 4, c.right - 12, c.top + 30};
        SelectObject(mdc, big);
        SetTextColor(mdc, RGB(235, 238, 245));
        DrawTextW(mdc, head, -1, &hr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        if (!lv.empty()) swprintf(head, 64, L"%d %ls", (int)lv.size(), lv.size() == 1 ? TR(L"vindu", L"window") : TR(L"vinduer", L"windows"));
        else swprintf(head, 64, L"%ls", TR(L"tom", L"empty"));
        SelectObject(mdc, smallBmp);
        SetTextColor(mdc, RGB(140, 146, 160));
        DrawTextW(mdc, head, -1, &hr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

        // the little screen
        RECT body{c.left + 10, c.top + 32, c.left + 10 + bodyW_, c.top + 32 + bodyH_};
        HBRUSH bb = CreateSolidBrush(RGB(20, 22, 26));
        FillRect(mdc, &body, bb);
        DeleteObject(bb);
        for (Node* l : lv) {
            RECT r{body.left + (int)((l->rect.left - spacesWa_.left) * (long long)bodyW_ / waW), body.top + (int)((l->rect.top - spacesWa_.top) * (long long)bodyH_ / waH),
                   body.left + (int)((l->rect.right - spacesWa_.left) * (long long)bodyW_ / waW), body.top + (int)((l->rect.bottom - spacesWa_.top) * (long long)bodyH_ / waH)};
            r.left = std::max(r.left, body.left); r.top = std::max(r.top, body.top);
            r.right = std::min(r.right, body.right); r.bottom = std::min(r.bottom, body.bottom);
            if (r.right - r.left < 4 || r.bottom - r.top < 4) continue;
            bool drawn = false;
            {
                std::lock_guard<std::mutex> lk(g_snap.mu);
                auto it = g_snap.raw.find(l->win);
                if (it != g_snap.raw.end() && !it->second.px.empty()) {
                    BITMAPINFO bi{};
                    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                    bi.bmiHeader.biWidth = it->second.w;
                    bi.bmiHeader.biHeight = -it->second.h;
                    bi.bmiHeader.biPlanes = 1;
                    bi.bmiHeader.biBitCount = 32;
                    SetStretchBltMode(mdc, HALFTONE);
                    SetBrushOrgEx(mdc, 0, 0, nullptr);
                    StretchDIBits(mdc, r.left, r.top, r.right - r.left, r.bottom - r.top, 0, 0, it->second.w, it->second.h,
                                  it->second.px.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
                    drawn = true;
                }
            }
            if (!drawn) {   // no picture yet: a plain tile with the title
                HBRUSH wb = CreateSolidBrush(RGB(66, 71, 86));
                FillRect(mdc, &r, wb);
                DeleteObject(wb);
                wchar_t t[80] = {};
                GetWindowTextW(l->win, t, 80);
                RECT tr{r.left + 4, r.top + 2, r.right - 4, r.bottom - 2};
                SelectObject(mdc, smallBmp);
                SetTextColor(mdc, RGB(200, 205, 218));
                DrawTextW(mdc, t, -1, &tr, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS);
            }
            HPEN bp = CreatePen(PS_SOLID, 1, RGB(96, 102, 120));
            SelectObject(mdc, bp);
            Rectangle(mdc, r.left, r.top, r.right, r.bottom);
            SelectObject(mdc, GetStockObject(NULL_PEN));
            DeleteObject(bp);
        }
    }
    BitBlt(dc, 0, 0, rc.right, rc.bottom, mdc, 0, 0, SRCCOPY);
    SelectObject(mdc, oldPen);
    SelectObject(mdc, oldBrush);
    SelectObject(mdc, oldFont);
    SelectObject(mdc, oldBm);
    DeleteObject(big);
    DeleteObject(smallBmp);
    DeleteObject(bm);
    DeleteDC(mdc);
    EndPaint(w, &ps);
}

LRESULT CALLBACK ETiler::OverviewProc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    ETiler* t = g_et.get();
    if (!t) return DefWindowProcW(w, m, wp, lp);
    switch (m) {
    case WM_PAINT: t->PaintOverview(w); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEMOVE: {
        int i = t->CardAt(lp);
        if (i != t->spacesHover_) { t->spacesHover_ = i; InvalidateRect(w, nullptr, FALSE); }
        TRACKMOUSEEVENT tm{sizeof tm, TME_LEAVE, w, 0};
        TrackMouseEvent(&tm);
        return 0;
    }
    case WM_MOUSELEAVE:
        t->spacesHover_ = -1;
        InvalidateRect(w, nullptr, FALSE);
        return 0;
    case WM_LBUTTONUP: {
        int i = t->CardAt(lp);
        HMONITOR mon = t->spacesMon_;
        t->HideOverview();
        if (i >= 0) t->SwitchSpace(mon, i + 1);
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) t->HideOverview();
        else if (wp >= '1' && wp <= '0' + kSpaces) { HMONITOR mon = t->spacesMon_; t->HideOverview(); t->SwitchSpace(mon, (int)(wp - '0')); }
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) t->HideOverview();
        return 0;
    }
    return DefWindowProcW(w, m, wp, lp);
}

std::vector<EnhancedSpaceInfo> ETiler::Info(HMONITOR mon, SIZE* waSize) {
    std::vector<EnhancedSpaceInfo> out;
    RECT wa = WorkArea(mon);
    float W = (float)std::max<LONG>(1, wa.right - wa.left), H = (float)std::max<LONG>(1, wa.bottom - wa.top);
    if (waSize) *waSize = {(LONG)W, (LONG)H};
    for (int n = 1; n <= kSpaces; ++n) {
        EnhancedSpaceInfo si{n, n == CurSpace(mon), {}};
        Tree* tree = nullptr;
        if (si.current) { auto t = trees_.find(mon); if (t != trees_.end()) tree = &t->second; }
        else { auto p = parked_.find({mon, n}); if (p != parked_.end()) tree = &p->second; }
        std::vector<Node*> lv;
        if (tree && tree->root) Leaves(tree->root.get(), lv);
        for (Node* l : lv)
            si.wins.push_back({l->win, (l->rect.left - wa.left) / W, (l->rect.top - wa.top) / H, (l->rect.right - wa.left) / W, (l->rect.bottom - wa.top) / H});
        out.push_back(std::move(si));
    }
    return out;
}

// Main window: the focused window takes the first place (swaps with whoever has it; if it already has it, with the next one).
void ETiler::PromoteMaster(HWND h) {
    HMONITOR mon = nullptr;
    Node* leaf = nullptr;
    if (!InTree(h, &mon, &leaf)) return;
    std::vector<Node*> lv;
    Leaves(trees_[mon].root.get(), lv);
    if (lv.size() < 2) return;
    Node* other = (leaf == lv[0]) ? lv[1] : lv[0];
    std::swap(leaf->win, other->win);
    placed_.clear();
    Retile();
}

void ETiler::SyncLocationHooks() {
    std::set<DWORD> want;
    for (HWND h : known_) {
        DWORD pid = 0;
        if (IsWindow(h) && GetWindowThreadProcessId(h, &pid) && pid) want.insert(pid);
    }
    for (auto it = locHooks_.begin(); it != locHooks_.end();) {
        if (want.count(it->first)) { ++it; continue; }
        UnhookWinEvent(it->second);
        it = locHooks_.erase(it);
    }
    for (DWORD pid : want) {
        if (locHooks_.count(pid)) continue;
        HWINEVENTHOOK h = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr, OnEvent,
                                          pid, 0, WINEVENT_OUTOFCONTEXT);
        if (h) locHooks_[pid] = h;
    }
}

// ---------------------------------------------------------------------------
// The user dragged an edge: move the divider that edge sits on (every divider the edge touches).
bool ETiler::ApplyResize(HWND h, const RECT& now) {
    HMONITOR mon = nullptr;
    Node* leaf = nullptr;
    if (!InTree(h, &mon, &leaf)) return false;
    auto tg = targets_.find(h);
    if (tg == targets_.end()) return false;
    const RECT T = tg->second;
    auto isAt = [](LONG a, LONG b) { return std::labs(a - b) <= 2; };
    auto moved = [](LONG a, LONG b) { return std::labs(a - b) > 2; };
    bool any = false;
    for (Node* c = leaf; c->parent; c = c->parent) {
        Node* p = c->parent;
        bool first = p->a.get() == c;
        LONG newMid = 0;
        bool hit = false;
        if (p->horiz) {
            if (first && isAt(T.right, p->firstEdge) && moved(now.right, T.right)) { newMid = now.right + (p->mid - p->firstEdge); hit = true; }
            else if (!first && isAt(T.left, p->secondEdge) && moved(now.left, T.left)) { newMid = now.left - (p->secondEdge - p->mid); hit = true; }
        } else {
            if (first && isAt(T.bottom, p->firstEdge) && moved(now.bottom, T.bottom)) { newMid = now.bottom + (p->mid - p->firstEdge); hit = true; }
            else if (!first && isAt(T.top, p->secondEdge) && moved(now.top, T.top)) { newMid = now.top - (p->secondEdge - p->mid); hit = true; }
        }
        if (!hit) continue;
        LONG lo = p->horiz ? p->area.left : p->area.top, hi = p->horiz ? p->area.right : p->area.bottom;
        if (hi <= lo) continue;
        p->ratio = std::clamp((float)(newMid - lo) / (float)(hi - lo), 0.1f, 0.9f);
        any = true;
    }
    return any;
}

// While an edge is dragged: the neighbours follow right away (at most about 60 times a second).
void ETiler::LiveResize(HWND h) {
    ULONGLONG t = GetTickCount64();
    if (t - lastLive_ < 16) return;
    HMONITOR mon = nullptr;
    if (!InTree(h, &mon)) return;
    if (!targets_.count(h)) return;
    RECT fb = FrameBounds(h);
    if (!dragResized_ && std::abs((fb.right - fb.left) - dragSize_.cx) <= 6 && std::abs((fb.bottom - fb.top) - dragSize_.cy) <= 6)
        return;   // only moved: it swaps when let go
    dragResized_ = true;
    lastLive_ = t;
    if (ApplyResize(h, fb)) { noAnim_ = true; Relayout(mon, h); noAnim_ = false; }
}

// ---------------------------------------------------------------------------
// Drop target: over another tiled window. The middle swaps; near an edge the dragged window
// splits that window and takes that side.  zone: 0 swap, 1 left, 2 right, 3 top, 4 bottom.
bool ETiler::HitTarget(HWND moved, POINT pt, HWND* tgt, int* zone, RECT* pv) const {
    for (auto& [h, r] : targets_) {
        if (h == moved || !PtInRect(&r, pt)) continue;
        float w = (float)std::max<LONG>(1, r.right - r.left), hh = (float)std::max<LONG>(1, r.bottom - r.top);
        float d[4] = {(pt.x - r.left) / w, (r.right - pt.x) / w, (pt.y - r.top) / hh, (r.bottom - pt.y) / hh};
        int best = 0;
        for (int i = 1; i < 4; ++i) if (d[i] < d[best]) best = i;
        int z = (useSnap_ && d[best] < 0.25f) ? best + 1 : 0;
        LONG mx = (r.left + r.right) / 2, my = (r.top + r.bottom) / 2;
        RECT p = r;
        if (z == 1) p.right = mx;
        else if (z == 2) p.left = mx;
        else if (z == 3) p.bottom = my;
        else if (z == 4) p.top = my;
        if (tgt) *tgt = h;
        if (zone) *zone = z;
        if (pv) *pv = p;
        return true;
    }
    return false;
}

void ETiler::DropAt(HWND moved, POINT pt) {
    HWND tgt = nullptr;
    int zone = 0;
    if (!HitTarget(moved, pt, &tgt, &zone, nullptr)) { placed_.erase(moved); return; }
    if (zone == 0) { SwapAt(moved, pt); return; }
    HMONITOR mm = nullptr, mt = nullptr;
    Node *lf = nullptr, *tl = nullptr;
    if (!InTree(moved, &mm, &lf) || !InTree(tgt, &mt, &tl)) { placed_.erase(moved); return; }
    RemoveLeaf(trees_[mm], lf);                  // (nodes may have moved: find the target again)
    if (!InTree(tgt, &mt, &tl)) { placed_.clear(); return; }
    SplitLeafAt(tl, moved, zone <= 2, zone == 1 || zone == 3);
    placed_.clear();
}

void ETiler::UpdateSnap(HWND h) {
    if (!useSnap_) return;
    ULONGLONG t = GetTickCount64();
    if (t - lastSnap_ < 30) return;
    lastSnap_ = t;
    POINT pt;
    GetCursorPos(&pt);
    HWND tgt = nullptr;
    int zone = 0;
    RECT pv{};
    if (InTree(h) && HitTarget(h, pt, &tgt, &zone, &pv)) ShowPreview(pv);
    else HidePreview();
}

void ETiler::ShowPreview(const RECT& r) {
    if (!preview_) {
        static bool reg = false;
        HINSTANCE hi = GetModuleHandleW(nullptr);
        if (!reg) {
            WNDCLASSEXW wc{sizeof wc};
            wc.lpfnWndProc = DefWindowProcW;
            wc.hInstance = hi;
            wc.hbrBackground = CreateSolidBrush(RGB(60, 120, 230));
            wc.lpszClassName = L"SkylineSnapPreview";
            RegisterClassExW(&wc);
            reg = true;
        }
        preview_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_TOPMOST,
                                   L"SkylineSnapPreview", L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, hi, nullptr);
        if (!preview_) return;
        SetLayeredWindowAttributes(preview_, 0, 95, LWA_ALPHA);
    }
    if (EqualRect(&r, &lastPreview_) && IsWindowVisible(preview_)) return;
    lastPreview_ = r;
    int w = r.right - r.left, h = r.bottom - r.top;
    SetWindowPos(preview_, HWND_TOPMOST, r.left, r.top, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SetWindowRgn(preview_, CreateRoundRectRgn(0, 0, w + 1, h + 1, 18, 18), TRUE);
}

void ETiler::HidePreview() {
    if (!preview_) return;
    ShowWindow(preview_, SW_HIDE);
    SetRectEmpty(&lastPreview_);
}

// ---------------------------------------------------------------------------
// Dividers: a thin, nearly invisible window in every gap. Hover lights it up; drag moves the divider live;
// double-click makes the two sides equal. (Needs a gap of a few pixels; at 0 a 6 px strip is used.)
bool ETiler::IsLiveNode(Node* n) {
    std::vector<Node*> stack;
    for (auto& [m, t] : trees_) if (t.root) stack.push_back(t.root.get());
    while (!stack.empty()) {
        Node* c = stack.back();
        stack.pop_back();
        if (c == n) return true;
        if (!c->leaf) { stack.push_back(c->a.get()); stack.push_back(c->b.get()); }
    }
    return false;
}

bool ETiler::MonOfNode(Node* n, HMONITOR* mon) {
    while (n->parent) n = n->parent;
    for (auto& [m, t] : trees_)
        if (t.root.get() == n) { *mon = m; return true; }
    return false;
}

void ETiler::DragDivider(Node* n, POINT pt) {
    HMONITOR mon = nullptr;
    if (!n || n->leaf || !MonOfNode(n, &mon)) return;
    ULONGLONG t = GetTickCount64();
    if (t - lastDiv_ < 16) return;
    lastDiv_ = t;
    LONG lo = n->horiz ? n->area.left : n->area.top, hi = n->horiz ? n->area.right : n->area.bottom;
    if (hi <= lo) return;
    LONG pos = n->horiz ? pt.x : pt.y;
    n->ratio = std::clamp((float)(pos - lo) / (float)(hi - lo), 0.1f, 0.9f);
    noAnim_ = true;   // live drag: follow the mouse directly
    Relayout(mon, nullptr);
    noAnim_ = false;
}

LRESULT CALLBACK ETiler::DivProc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    ETiler* t = g_et.get();
    Node* n = (Node*)GetWindowLongPtrW(w, GWLP_USERDATA);
    switch (m) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_SETCURSOR:
        if (n) { SetCursor(LoadCursorW(nullptr, n->horiz ? IDC_SIZEWE : IDC_SIZENS)); return TRUE; }
        break;
    case WM_MOUSEMOVE:
        if (!t) break;
        if (!t->divHover_) {
            TRACKMOUSEEVENT tm{sizeof tm, TME_LEAVE, w, 0};
            TrackMouseEvent(&tm);
            t->divHover_ = true;
            SetLayeredWindowAttributes(w, 0, 120, LWA_ALPHA);
        }
        if (t->divDrag_ == w && n) { POINT p; GetCursorPos(&p); t->DragDivider(n, p); }
        return 0;
    case WM_MOUSELEAVE:
        if (t) t->divHover_ = false;
        if (!t || t->divDrag_ != w) SetLayeredWindowAttributes(w, 0, 1, LWA_ALPHA);
        return 0;
    case WM_LBUTTONDOWN:
        if (t && n && t->IsLiveNode(n)) {
            SetCapture(w);
            t->divDrag_ = w;
            g_moving = w;   // nothing re-arranges while the divider is held
            SetLayeredWindowAttributes(w, 0, 180, LWA_ALPHA);
        }
        return 0;
    case WM_LBUTTONUP:
        ReleaseCapture();   // WM_CAPTURECHANGED does the rest
        return 0;
    case WM_CAPTURECHANGED:
        if (t && t->divDrag_ == w) {
            t->divDrag_ = nullptr;
            if (g_moving == w) g_moving = nullptr;
            SetLayeredWindowAttributes(w, 0, t->divHover_ ? 120 : 1, LWA_ALPHA);
            t->placed_.clear();
            t->Schedule(30);
        }
        return 0;
    case WM_LBUTTONDBLCLK:
        if (t && n && t->IsLiveNode(n)) {
            HMONITOR mon = nullptr;
            if (t->MonOfNode(n, &mon)) { n->ratio = 0.5f; t->Relayout(mon, nullptr); }
        }
        return 0;
    }
    return DefWindowProcW(w, m, wp, lp);
}

void ETiler::HideDividers() {
    for (HWND d : divs_) { SetWindowLongPtrW(d, GWLP_USERDATA, 0); ShowWindow(d, SW_HIDE); }
}

void ETiler::DestroyDividers() {
    for (HWND d : divs_) DestroyWindow(d);
    divs_.clear();
    divHover_ = false;
}

void ETiler::UpdateDividers() {
    if (!running_ || !useDividers_) { if (!divs_.empty()) DestroyDividers(); return; }
    // A maximized (or fullscreen) window in front of a screen: no dividers there, they'd float on top of it.
    auto Alive = [](Node* n) {   // is there at least one shown, normal window below this node?
        std::vector<Node*> st{n};
        while (!st.empty()) {
            Node* c = st.back();
            st.pop_back();
            if (c->leaf) { if (c->win && IsWindow(c->win) && IsWindowVisible(c->win) && !IsIconic(c->win) && !IsZoomed(c->win)) return true; }
            else { st.push_back(c->a.get()); st.push_back(c->b.get()); }
        }
        return false;
    };
    std::set<HMONITOR> covered;
    {
        struct Cx { std::set<HMONITOR>* cov; std::set<HMONITOR> seen; const ETiler* self; } cx{&covered, {}, this};
        EnumWindows([](HWND h, LPARAM lp) -> BOOL {   // top to bottom: the first real window per screen decides
            auto* c = reinterpret_cast<Cx*>(lp);
            if (!IsWindowVisible(h) || IsIconic(h)) return TRUE;
            LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
            if (ex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) return TRUE;   // dividers, bars, docks, popups
            int cloaked = 0;
            DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof cloaked);
            if (cloaked) return TRUE;
            wchar_t cls[64] = L"";
            GetClassNameW(h, cls, 64);
            if (!lstrcmpW(cls, L"Progman") || !lstrcmpW(cls, L"WorkerW")) return TRUE;
            HMONITOR m = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
            if (c->seen.count(m)) return TRUE;
            c->seen.insert(m);
            bool full = IsZoomed(h);
            if (!full) {
                MONITORINFO mi{sizeof mi};
                RECT r;
                if (GetMonitorInfoW(m, &mi) && GetWindowRect(h, &r))
                    full = r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top &&
                           r.right >= mi.rcMonitor.right && r.bottom >= mi.rcMonitor.bottom;
            }
            if (full) c->cov->insert(m);
            return TRUE;
        }, (LPARAM)&cx);
    }
    std::vector<Node*> splits;
    for (auto& [m, t] : trees_) {
        if (covered.count(m)) continue;
        std::vector<Node*> stack;
        if (t.root) stack.push_back(t.root.get());
        while (!stack.empty()) {
            Node* c = stack.back();
            stack.pop_back();
            if (c->leaf) continue;
            if (!Alive(c->a.get()) || !Alive(c->b.get())) continue;   // a divider needs a real window on both sides
            splits.push_back(c);
            stack.push_back(c->a.get());
            stack.push_back(c->b.get());
        }
    }
    static bool reg = false;
    HINSTANCE hi = GetModuleHandleW(nullptr);
    if (!reg) {
        WNDCLASSEXW wc{sizeof wc};
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = DivProc;
        wc.hInstance = hi;
        wc.hbrBackground = CreateSolidBrush(RGB(120, 160, 230));
        wc.lpszClassName = L"SkylineDivider";
        RegisterClassExW(&wc);
        reg = true;
    }
    while (divs_.size() < splits.size()) {
        HWND d = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, L"SkylineDivider", L"",
                                 WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, hi, nullptr);
        if (!d) break;
        SetLayeredWindowAttributes(d, 0, 1, LWA_ALPHA);   // 1/255: invisible but still catches the mouse
        divs_.push_back(d);
    }
    for (size_t i = 0; i < divs_.size(); ++i) {
        HWND d = divs_[i];
        if (i >= splits.size()) { SetWindowLongPtrW(d, GWLP_USERDATA, 0); ShowWindow(d, SW_HIDE); continue; }
        Node* n = splits[i];
        RECT r;
        const LONG minW = 6;
        if (n->horiz) {
            r = {n->firstEdge, n->area.top, n->secondEdge, n->area.bottom};
            if (r.right - r.left < minW) { LONG c = n->mid; r.left = c - minW / 2; r.right = c + minW / 2; }
        } else {
            r = {n->area.left, n->firstEdge, n->area.right, n->secondEdge};
            if (r.bottom - r.top < minW) { LONG c = n->mid; r.top = c - minW / 2; r.bottom = c + minW / 2; }
        }
        SetWindowLongPtrW(d, GWLP_USERDATA, (LONG_PTR)n);
        SetWindowPos(d, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
}

// Dropping a tiled window onto another one on the same screen swaps them.
void ETiler::SwapAt(HWND moved, POINT pt) {
    HMONITOR mon = nullptr;
    Node* from = nullptr;
    if (!InTree(moved, &mon, &from)) return;
    if (MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST) == mon) {
        for (auto& [h, r] : targets_) {
            if (h == moved || !PtInRect(&r, pt)) continue;
            Node* to = FindLeaf(trees_[mon].root.get(), h);
            if (to) std::swap(from->win, to->win);
            break;
        }
    }
    placed_.erase(moved);
}

void ETiler::ToggleFloat(HWND h) {
    if (!h) return;
    if (floating_.count(h)) floating_.erase(h);
    else { floating_.insert(h); placed_.erase(h); }
    Schedule(10);
}

HWND ETiler::Neighbor(HWND from, int dx, int dy) const {
    auto it = targets_.find(from);
    if (it == targets_.end()) return nullptr;
    POINT c = Center(it->second);
    HWND best = nullptr;
    long bestScore = LONG_MAX;
    for (auto& [h, r] : targets_) {
        if (h == from) continue;
        POINT o = Center(r);
        long ax = o.x - c.x, ay = o.y - c.y;
        if ((dx && ax * dx <= 0) || (dy && ay * dy <= 0)) continue;
        long score = dx ? labs(ax) + 2 * labs(ay) : labs(ay) + 2 * labs(ax);
        if (score < bestScore) { bestScore = score; best = h; }
    }
    return best;
}

// ---------------------------------------------------------------------------
// Hotkeys (only when the user chose "Automatic + shortcuts"): Ctrl+Alt (+Shift) arrows as in plain tiling,
// and Ctrl+Alt+Win+arrows to move the divider nearest the window.
void ETiler::RegisterHotkeys() {
    HWND w = g_app.ctlWnd;
    const UINT ca = MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, cas = ca | MOD_SHIFT, caw = ca | MOD_WIN;
    RegisterHotKey(w, HkFocusL, ca, VK_LEFT);
    RegisterHotKey(w, HkFocusR, ca, VK_RIGHT);
    RegisterHotKey(w, HkFocusU, ca, VK_UP);
    RegisterHotKey(w, HkFocusD, ca, VK_DOWN);
    RegisterHotKey(w, HkSwapL, cas, VK_LEFT);
    RegisterHotKey(w, HkSwapR, cas, VK_RIGHT);
    RegisterHotKey(w, HkSwapU, cas, VK_UP);
    RegisterHotKey(w, HkSwapD, cas, VK_DOWN);
    const UINT rep = MOD_CONTROL | MOD_ALT | MOD_WIN;   // no NOREPEAT: hold to keep resizing
    RegisterHotKey(w, HkGrowL, rep, VK_LEFT);
    RegisterHotKey(w, HkGrowR, rep, VK_RIGHT);
    RegisterHotKey(w, HkGrowU, rep, VK_UP);
    RegisterHotKey(w, HkGrowD, rep, VK_DOWN);
    RegisterHotKey(w, HkEqual, caw, 'E');
    RegisterHotKey(w, HkFloat, ca, 'F');
    RegisterHotKey(w, HkRetile, ca, VK_SPACE);
    RegisterHotKey(w, HkMaster, ca, VK_RETURN);
    RegisterHotKey(w, HkOverview, ca, 'O');          // Ctrl+Alt+O: overview of all workspaces
    for (int i = 0; i < kSpaces; ++i) {
        RegisterHotKey(w, HkSpace1 + i, ca, '1' + i);     // Ctrl+Alt+1-4: go to workspace
        RegisterHotKey(w, HkSend1 + i, cas, '1' + i);     // Ctrl+Alt+Shift+1-4: send the window there
    }
    hotkeys_ = true;
}

void ETiler::UnregisterHotkeys() {
    for (int id = HkFocusL; id <= HkLast; ++id) UnregisterHotKey(g_app.ctlWnd, id);
    hotkeys_ = false;
}

bool ETiler::OnHotkey(int id) {
    if (id < HkFocusL || id > HkLast) return false;
    if (!running_) return true;
    HWND fg = GetForegroundWindow();
    int dx = 0, dy = 0;
    switch (id) {
    case HkFocusL: case HkSwapL: case HkGrowL: dx = -1; break;
    case HkFocusR: case HkSwapR: case HkGrowR: dx = 1; break;
    case HkFocusU: case HkSwapU: case HkGrowU: dy = -1; break;
    case HkFocusD: case HkSwapD: case HkGrowD: dy = 1; break;
    }
    switch (id) {
    case HkFocusL: case HkFocusR: case HkFocusU: case HkFocusD:
        if (HWND n = Neighbor(fg, dx, dy)) SetForegroundWindow(n);
        break;
    case HkSwapL: case HkSwapR: case HkSwapU: case HkSwapD:
        if (HWND n = Neighbor(fg, dx, dy)) {
            HMONITOR m1 = nullptr, m2 = nullptr;
            Node *a = nullptr, *b = nullptr;
            if (InTree(fg, &m1, &a) && InTree(n, &m2, &b)) { std::swap(a->win, b->win); placed_.clear(); Retile(); }
        }
        break;
    case HkGrowL: case HkGrowR: case HkGrowU: case HkGrowD: {
        // The arrow moves the divider nearest the window (on that axis) in that direction.
        Node* leaf = nullptr;
        if (!InTree(fg, nullptr, &leaf)) break;
        for (Node* c = leaf; c->parent; c = c->parent) {
            Node* p = c->parent;
            if (p->horiz != (dx != 0)) continue;
            p->ratio = std::clamp(p->ratio + 0.04f * (float)(dx + dy), 0.1f, 0.9f);
            break;
        }
        placed_.clear();
        Retile();
        break;
    }
    case HkEqual: {
        HMONITOR m = nullptr;
        if (InTree(fg, &m)) Equalize(trees_[m].root.get());
        else for (auto& [mon, t] : trees_) Equalize(t.root.get());
        placed_.clear();
        Retile();
        break;
    }
    case HkFloat:
        ToggleFloat(fg);
        break;
    case HkRetile:
        placed_.clear();
        Retile();
        break;
    case HkMaster:
        PromoteMaster(fg);
        break;
    case HkOverview: {
        POINT c;
        GetCursorPos(&c);
        ToggleOverview(MonitorFromPoint(c, MONITOR_DEFAULTTONEAREST));
        break;
    }
    case HkSpace1: case HkSpace2: case HkSpace3: case HkSpace4: case HkSpace5: case HkSpace6: case HkSpace7: case HkSpace8: {
        POINT c;
        GetCursorPos(&c);
        SwitchSpace(MonitorFromPoint(c, MONITOR_DEFAULTTONEAREST), id - HkSpace1 + 1);
        break;
    }
    case HkSend1: case HkSend2: case HkSend3: case HkSend4: case HkSend5: case HkSend6: case HkSend7: case HkSend8:
        SendToSpace(fg, id - HkSend1 + 1);
        break;
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Public functions (see enhancedtiling.h)
void EnhancedApply() {
    if (!g_et) g_et = std::make_unique<ETiler>();
    auto plain = [] {
        if (!g_app.tiler) return;
        if (g_app.settings.enhancedTiling) {
            // plain tiling stays off (and its shortcuts unregistered) while Enhanced Tiling is chosen
            bool e = g_app.settings.tilingEnabled;
            int m = g_app.settings.tilingMode;
            g_app.settings.tilingEnabled = false;
            g_app.settings.tilingMode = 0;
            g_app.tiler->ApplySettings();
            g_app.settings.tilingEnabled = e;
            g_app.settings.tilingMode = m;
        } else {
            g_app.tiler->ApplySettings();
        }
    };
    // Whichever engine is being switched off goes first, so two never fight over the windows.
    if (g_app.settings.enhancedTiling) { plain(); g_et->ApplySettings(); }
    else { g_et->ApplySettings(); plain(); }
}

void EnhancedShutdown() {
    if (!g_et) return;
    g_et->Shutdown();
    g_et.reset();
}

bool EnhancedManages(HWND h) { return g_et && g_et->Manages(h); }
bool EnhancedIsHidden(HWND h) { return g_et && g_et->IsHiddenWin(h); }
bool EnhancedSpacesAvailable() { return g_app.settings.tilingEnabled && g_app.settings.enhancedTiling; }
int EnhancedCurrentSpace(HMONITOR mon) { return g_et ? g_et->CurrentSpace(mon) : 1; }
void EnhancedSpacesToggle(HMONITOR mon) { if (g_et) g_et->ToggleOverview(mon); }
std::vector<EnhancedSpaceInfo> EnhancedSpacesInfo(HMONITOR mon, SIZE* sz) { return (g_et && g_et->Running()) ? g_et->Info(mon, sz) : std::vector<EnhancedSpaceInfo>(); }
void EnhancedSwitchSpace(HMONITOR mon, int n) { if (g_et && g_et->Running()) g_et->GoToSpace(mon, n); }
bool EnhancedReveal(HWND h) { return g_et && g_et->Reveal(h); }
ComPtr<IWICBitmapSource> EnhancedSnapshot(HWND h) { return RawToWic(h); }   // (works without Enhanced Tiling too)
void EnhancedSnapshotKick(HWND h, bool force) { if (h) KickCapture({h}, force); }
void EnhancedTimer() { if (g_et) g_et->OnTimer(); }
void EnhancedAnimTimer() { if (g_et) g_et->OnAnimTimer(); }
void EnhancedFocusTimer() { if (g_et) g_et->OnFocusTimer(); }
bool EnhancedHotkey(int id) { return g_et && g_et->OnHotkey(id); }
void EnhancedFlushPending() { if (g_et) g_et->FlushPending(); }

bool TilingRunning() { return (g_et && g_et->Running()) || (g_app.tiler && g_app.tiler->Running()); }
bool TilingIsFloating(HWND h) { return (g_et && g_et->Running()) ? g_et->IsFloating(h) : (g_app.tiler && g_app.tiler->IsFloating(h)); }
void TilingToggleFloat(HWND h) {
    if (g_et && g_et->Running()) g_et->ToggleFloat(h);
    else if (g_app.tiler) g_app.tiler->ToggleFloat(h);
}
void TilingSchedule(UINT ms) {
    if (g_et && g_et->Running()) g_et->Schedule(ms);
    else if (g_app.tiler) g_app.tiler->Schedule(ms);
}

// The "+" in Mission Control: one more workspace on every screen (kept in skyline.ini).
bool EnhancedAddSpace() {
    if (!g_et || !g_et->Running() || g_nSpaces >= 8) return false;
    ++g_nSpaces;
    g_et->ReRegisterHotkeys();
    WritePrivateProfileStringW(L"Tiling", L"Spaces", std::to_wstring(g_nSpaces).c_str(), g_app.settings.path.c_str());
    return true;
}

// The "x" on the last workspace in Mission Control: its windows move to the workspace before it.
bool EnhancedCanRemoveSpace() { return g_et && g_et->Running() && g_nSpaces > 2; }
bool EnhancedRemoveSpace() {
    if (!EnhancedCanRemoveSpace()) return false;
    g_et->DropLastSpace();
    --g_nSpaces;
    g_et->ReRegisterHotkeys();
    WritePrivateProfileStringW(L"Tiling", L"Spaces", std::to_wstring(g_nSpaces).c_str(), g_app.settings.path.c_str());
    return true;
}
