// Skyline — Copyright © 2026 Yen-Zen & Cyber. All rights reserved. See LICENSE.txt.
// Built-in updates. A small manifest on skyline.skybyn.com names the newest
// version, its download and its SHA-256, and is signed with an ECDSA P-256 key
// that only lives on the developers' PCs (tools/sign-release.ps1). Skyline
// checks it now and then, asks, and on "Update" downloads the exe, checks the
// hash, swaps it in for the running Skyline.exe and restarts. Whoever controls
// the web server still can't push code to users without the key.
//
// Manifest (UTF-8, LF):
//   skyline-update 1
//   version=2.2.0
//   url=https://skyline.skybyn.com/update/Skyline-2.2.0.exe
//   size=924160
//   sha256=<64 hex>
//   notes=One line, English
//   notes_no=Én linje, norsk
//   sig=<128 hex: P1363 r||s over every byte before "sig=">
#include "update.h"
#include "dialog.h"
#include "app.h"
#include "settings.h"
#include "version.h"
#include "update_key.h"
#include <winhttp.h>
#include <bcrypt.h>

namespace {

constexpr wchar_t kManifestUrl[] = L"https://skyline.skybyn.com/update/latest.txt";
constexpr wchar_t kClass[] = L"SkylineUpdate";
constexpr UINT_PTR T_FIRST = 1, T_REPEAT = 2, T_PROMPT = 3;
constexpr UINT kFirstDelay = 45 * 1000;              // let the PC finish signing in first
constexpr UINT kRepeat = 6 * 60 * 60 * 1000;
constexpr UINT kPromptRetry = 10 * 60 * 1000;        // a game was fullscreen: ask later
constexpr size_t kMaxManifest = 16 * 1024, kMaxExe = 64 * 1024 * 1024;
constexpr UINT WM_RESULT = WM_APP + 1;               // our own window only
// What any Skyline exe (2.2.0 and later) answers to --update-selftest. Older
// versions start the new exe this way before installing it, so keep it forever.
constexpr int kSelfTestCode = 0x534B;

struct Manifest {
    std::wstring version, url, notes, notesNo;
    size_t size = 0;
    std::string sha256;                              // lowercase hex
};

struct Result {
    enum Kind { Checked, Downloaded, Installed } kind;
    bool ok = false;
    std::wstring error;
    Manifest m;
    std::wstring file;
};

HWND g_wnd = nullptr;
bool g_justUpdated = false;
std::wstring g_exe;                                  // our own path, captured before any rename
Update::State g_state = Update::State::Idle;
Manifest g_avail;
std::wstring g_error;
std::wstring g_lastChecked;

// ---------------------------------------------------------------------------
std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::wstring ModulePath() {
    wchar_t p[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, p, MAX_PATH * 2);
    return std::wstring(p, n);
}
std::wstring OldFile() { return ExeDir() + L"\\Skyline.old.exe"; }
std::wstring TempFile() {
    wchar_t t[MAX_PATH + 1];
    GetTempPathW(MAX_PATH + 1, t);
    return std::wstring(t) + L"Skyline-update.exe";
}

bool FromHex(const std::string& s, BYTE* out, size_t n) {
    if (s.size() != n * 2) return false;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < n; ++i) {
        int hi = nib(s[2 * i]), lo = nib(s[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (BYTE)(hi << 4 | lo);
    }
    return true;
}

std::string ToHex(const BYTE* p, size_t n) {
    static const char d[] = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) { s += d[p[i] >> 4]; s += d[p[i] & 15]; }
    return s;
}

std::string Sha256Hex(const void* data, size_t n) {
    BYTE h[32];
    if (!BCRYPT_SUCCESS(BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, (PUCHAR)data, (ULONG)n, h, sizeof h))) return {};
    return ToHex(h, sizeof h);
}

bool VerifySignature(const std::string& body, const BYTE sig[64]) {
    BYTE h[32];
    if (!BCRYPT_SUCCESS(BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, (PUCHAR)body.data(), (ULONG)body.size(), h, sizeof h)))
        return false;
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    bool ok = false;
    if (BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0)) &&
        BCRYPT_SUCCESS(BCryptImportKeyPair(alg, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key, (PUCHAR)kUpdateKey, sizeof kUpdateKey, 0)))
        ok = BCRYPT_SUCCESS(BCryptVerifySignature(key, nullptr, h, sizeof h, (PUCHAR)sig, 64, 0));
    if (key) BCryptDestroyKey(key);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

bool ParseVersion(const std::wstring& s, int v[3]) {
    wchar_t tail = 0;
    return swscanf(s.c_str(), L"%d.%d.%d%lc", &v[0], &v[1], &v[2], &tail) == 3 && v[0] >= 0 && v[1] >= 0 && v[2] >= 0;
}

bool IsNewer(const std::wstring& version) {
    int a[3], b[3] = {SKYLINE_VERSION_MAJOR, SKYLINE_VERSION_MINOR, SKYLINE_VERSION_PATCH};
    if (!ParseVersion(version, a)) return false;
    for (int i = 0; i < 3; ++i)
        if (a[i] != b[i]) return a[i] > b[i];
    return false;
}

// ---------------------------------------------------------------------------
// HTTPS GET into memory. Only https:// is accepted, and WinHTTP refuses to
// follow a redirect from https to http.
bool HttpGet(const std::wstring& url, size_t maxBytes, std::string& out, std::wstring& err) {
    wchar_t host[256], path[2048], extra[1024];
    URL_COMPONENTS u{sizeof u};
    u.lpszHostName = host; u.dwHostNameLength = ARRAYSIZE(host);
    u.lpszUrlPath = path; u.dwUrlPathLength = ARRAYSIZE(path);
    u.lpszExtraInfo = extra; u.dwExtraInfoLength = ARRAYSIZE(extra);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &u) || u.nScheme != INTERNET_SCHEME_HTTPS) {
        err = TR(L"Ugyldig adresse til oppdateringen", L"Invalid update address");
        return false;
    }
    std::wstring object = std::wstring(path) + extra;
    bool ok = false;
    DWORD status = 0, size = sizeof status;
    HINTERNET s = WinHttpOpen(L"Skyline/" SKYLINE_VERSION_W, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET c = s ? WinHttpConnect(s, host, u.nPort, 0) : nullptr;
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    if (s) WinHttpSetTimeouts(s, 10000, 10000, 15000, 30000);
    if (r && WinHttpSendRequest(r, L"Cache-Control: no-cache\r\n", (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(r, nullptr) &&
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &status, &size, WINHTTP_NO_HEADER_INDEX)) {
        if (status != 200) {
            err = TR(L"Serveren svarte ", L"The server answered ") + std::to_wstring(status);
        } else {
            out.clear();
            ok = true;
            for (;;) {
                DWORD avail = 0, got = 0;
                if (!WinHttpQueryDataAvailable(r, &avail)) { ok = false; break; }
                if (!avail) break;
                if (out.size() + avail > maxBytes) {
                    err = TR(L"Nedlastingen er for stor", L"The download is too large");
                    ok = false;
                    break;
                }
                size_t at = out.size();
                out.resize(at + avail);
                if (!WinHttpReadData(r, out.data() + at, avail, &got)) { ok = false; break; }
                out.resize(at + got);
            }
        }
    }
    if (!ok && err.empty())
        err = TR(L"Fikk ikke kontakt med oppdateringsserveren (feil ", L"Couldn't reach the update server (error ") +
              std::to_wstring(GetLastError()) + L")";
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    return ok;
}

bool ParseManifest(const std::string& text, Manifest& m, std::wstring& err) {
    size_t at = text.find("\nsig=");
    BYTE sig[64];
    if (text.rfind("skyline-update 1\n", 0) != 0 || at == std::string::npos) {
        err = TR(L"Oppdateringsinformasjonen er ugyldig", L"The update information is invalid");
        return false;
    }
    std::string body = text.substr(0, at + 1);       // everything the signature covers
    if (!FromHex(text.substr(at + 5, 128), sig, sizeof sig) || !VerifySignature(body, sig)) {
        err = TR(L"Oppdateringen er ikke signert av Skyline, og ble avvist", L"The update isn't signed by Skyline and was rejected");
        return false;
    }
    // Only ever read values from the signed part.
    for (size_t i = 0; i < body.size();) {
        size_t e = body.find('\n', i);
        std::string line = body.substr(i, e - i);
        i = e + 1;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "version") m.version = Widen(v);
        else if (k == "url") m.url = Widen(v);
        else if (k == "size") m.size = (size_t)strtoull(v.c_str(), nullptr, 10);
        else if (k == "sha256") { m.sha256 = v; for (auto& ch : m.sha256) ch = (char)tolower((unsigned char)ch); }
        else if (k == "notes") m.notes = Widen(v);
        else if (k == "notes_no") m.notesNo = Widen(v);
    }
    int v[3];
    BYTE h[32];
    if (!ParseVersion(m.version, v) || m.url.empty() || !m.size || m.size > kMaxExe || !FromHex(m.sha256, h, sizeof h)) {
        err = TR(L"Oppdateringsinformasjonen er ufullstendig", L"The update information is incomplete");
        return false;
    }
    return true;
}

bool ReadAll(const std::wstring& path, std::string& out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER n{};
    bool ok = GetFileSizeEx(f, &n) && n.QuadPart <= (LONGLONG)kMaxExe;
    DWORD got = 0;
    if (ok) {
        out.resize((size_t)n.QuadPart);
        ok = ::ReadFile(f, out.data(), (DWORD)out.size(), &got, nullptr) && got == out.size();
    }
    CloseHandle(f);
    return ok;
}

// Returns 0 or the error. flush = on disk before we return (it's about to replace Skyline.exe).
DWORD WriteAll(const std::wstring& path, const std::string& data, bool flush) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return GetLastError();
    DWORD put = 0, e = ERROR_SUCCESS;
    if (!::WriteFile(f, data.data(), (DWORD)data.size(), &put, nullptr) || put != data.size() || (flush && !FlushFileBuffers(f)))
        e = GetLastError() ? GetLastError() : ERROR_WRITE_FAULT;
    CloseHandle(f);
    if (e) DeleteFileW(path.c_str());
    return e;
}

// Puts the verified new exe in place of the running one. A running exe can be
// renamed but not deleted or overwritten, so: write the new one next to it
// (same volume, flushed), then ReplaceFile, which renames the running exe to
// Skyline.old.exe and the new one into place in one step and keeps the
// original's permissions. There is never a half-written Skyline.exe.
DWORD Apply(const std::wstring& src, const std::string& sha, const std::wstring& target) {
    std::string data;
    if (!ReadAll(src, data)) return ERROR_FILE_NOT_FOUND;
    if (Sha256Hex(data.data(), data.size()) != sha) return ERROR_INVALID_DATA;   // checked again: src sits in %TEMP%
    std::wstring dir = target.substr(0, target.find_last_of(L'\\'));
    std::wstring staged = dir + L"\\Skyline.new.exe", backup = dir + L"\\Skyline.old.exe";
    if (DWORD e = WriteAll(staged, data, true)) return e;
    DeleteFileW(backup.c_str());   // the previous update's backup
    if (ReplaceFileW(target.c_str(), staged.c_str(), backup.c_str(), REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr))
        return ERROR_SUCCESS;
    DWORD e = GetLastError();
    if (e == ERROR_UNABLE_TO_MOVE_REPLACEMENT_2) MoveFileExW(backup.c_str(), target.c_str(), 0);   // put the old one back
    DeleteFileW(staged.c_str());
    return e;
}

// Starts the downloaded exe in a do-nothing mode before anything is replaced.
// Smart App Control and antivirus block or remove unsigned exes they haven't
// seen before; better to find out now than after the old one is gone.
DWORD SelfTest(const std::wstring& exe) {
    std::wstring cmd = L"\"" + exe + L"\" --update-selftest";
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return GetLastError();
    DWORD code = ERROR_TIMEOUT;
    if (WaitForSingleObject(pi.hProcess, 20000) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    else TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code == (DWORD)kSelfTestCode ? ERROR_SUCCESS : ERROR_BAD_EXE_FORMAT;
}

// ---------------------------------------------------------------------------
// Workers. Each one posts a heap Result back to g_wnd.
void RunWorker(std::function<void()> fn) {
    auto* p = new std::function<void()>(std::move(fn));
    HANDLE t = CreateThread(nullptr, 0, [](void* a) -> DWORD {
        auto* f = (std::function<void()>*)a;
        (*f)();
        delete f;
        return 0;
    }, p, 0, nullptr);
    if (t) CloseHandle(t);
    else delete p;
}

void Post(Result* r) {
    if (!PostMessageW(g_wnd, WM_RESULT, 0, (LPARAM)r)) delete r;
}

void CheckWorker() {
    auto* r = new Result{Result::Checked};
    std::string text;
    r->ok = HttpGet(kManifestUrl, kMaxManifest, text, r->error) && ParseManifest(text, r->m, r->error);
    Post(r);
}

void DownloadWorker(Manifest m) {
    auto* r = new Result{Result::Downloaded};
    std::string data;
    if (HttpGet(m.url, kMaxExe, data, r->error)) {
        if (data.size() != m.size || data.compare(0, 2, "MZ") != 0 || Sha256Hex(data.data(), data.size()) != m.sha256)
            r->error = TR(L"Den nedlastede filen stemmer ikke med signaturen, og ble forkastet",
                          L"The downloaded file doesn't match the signature and was discarded");
        else if (WriteAll(TempFile(), data, false))
            r->error = TR(L"Kunne ikke lagre oppdateringen", L"Couldn't save the update");
        else { r->ok = true; r->file = TempFile(); }
    }
    r->m = std::move(m);
    Post(r);
}

void InstallWorker(std::wstring file, std::string sha, std::wstring exe) {
    auto* r = new Result{Result::Installed};
    DWORD e = SelfTest(file);
    if (e != ERROR_SUCCESS) {
        r->error = TR(L"Windows lot ikke den nye versjonen starte (Smart App Control eller antivirus?). Ingenting er endret.",
                      L"Windows wouldn't let the new version start (Smart App Control or antivirus?). Nothing was changed.");
        Post(r);
        return;
    }
    e = Apply(file, sha, exe);
    if (e == ERROR_ACCESS_DENIED || e == ERROR_PRIVILEGE_NOT_HELD) {
        // Not ours to write (Program Files): ask Windows for admin once, for the swap only.
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        std::wstring args = L"--apply-update \"" + file + L"\" " + std::wstring(sha.begin(), sha.end());
        SHELLEXECUTEINFOW sei{sizeof sei};
        sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
        sei.lpVerb = L"runas";
        sei.lpFile = exe.c_str();
        sei.lpParameters = args.c_str();
        sei.nShow = SW_HIDE;
        if (!ShellExecuteExW(&sei)) e = GetLastError();
        else if (!sei.hProcess) e = ERROR_GEN_FAILURE;
        else {
            DWORD code = ERROR_GEN_FAILURE;
            if (WaitForSingleObject(sei.hProcess, 60000) != WAIT_OBJECT_0) code = ERROR_TIMEOUT;
            else GetExitCodeProcess(sei.hProcess, &code);
            e = code;
            CloseHandle(sei.hProcess);
        }
        CoUninitialize();
    }
    r->ok = e == ERROR_SUCCESS;
    if (e == ERROR_CANCELLED)
        r->error = TR(L"Oppdateringen ble avbrutt", L"The update was cancelled");
    else if (!r->ok)
        r->error = TR(L"Kunne ikke bytte ut Skyline.exe (feil ", L"Couldn't replace Skyline.exe (error ") + std::to_wstring(e) + L")";
    Post(r);
}

// ---------------------------------------------------------------------------
void SetState(Update::State s) {
    g_state = s;
    SettingsChanged();   // the app menu reads the state each time it opens
}

void Check() {
    if (g_state == Update::State::Checking || g_state == Update::State::Downloading || g_state == Update::State::Installing)
        return;
    if (!Update::HasKey()) return;
    g_error.clear();
    SetState(Update::State::Checking);
    RunWorker(CheckWorker);
}

std::wstring IniString(const wchar_t* sec, const wchar_t* key) {
    wchar_t buf[64];
    GetPrivateProfileStringW(sec, key, L"", buf, ARRAYSIZE(buf), g_app.settings.path.c_str());
    return buf;
}

// Ask once per new version, never on top of a fullscreen game.
void MaybePrompt() {
    KillTimer(g_wnd, T_PROMPT);
    if (g_state != Update::State::Available || IniString(L"Update", L"Notified") == g_avail.version) return;
    if (PrimaryFullscreen()) { SetTimer(g_wnd, T_PROMPT, kPromptRetry, nullptr); return; }
    IniWrite(L"Update", L"Notified", g_avail.version);
    Dialog::Spec d;
    d.caption = TR(L"Skyline – oppdatering", L"Skyline – update");
    d.heading = TR(L"Skyline ", L"Skyline ") + g_avail.version + TR(L" er klar", L" is available");
    d.body = Update::Notes();
    d.note = TR(L"Du har " SKYLINE_VERSION_W L". Skyline starter på nytt med en gang. Du kan også oppdatere senere fra appnavnet eller Innstillinger → Om Skyline.",
                L"You have " SKYLINE_VERSION_W L". Skyline restarts right away. You can also update later from the app name or Settings → About Skyline.");
    d.primary = TR(L"Oppdater nå", L"Update now");
    d.secondary = TR(L"Senere", L"Later");
    KillTimer(g_wnd, T_REPEAT);   // no check may start while the question is open
    Dialog::Show(d, [](bool yes) {
        SetTimer(g_wnd, T_REPEAT, kRepeat, nullptr);
        if (yes) Update::Install();
    });
}

void Restart() {
    std::wstring cmd = L"\"" + g_exe + L"\" --after-update";
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    std::wstring dir = g_exe.substr(0, g_exe.find_last_of(L'\\'));
    if (!CreateProcessW(g_exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi)) {
        g_error = TR(L"Oppdatert, men kunne ikke starte på nytt. Start Skyline igjen selv.",
                     L"Updated, but couldn't restart. Start Skyline again yourself.");
        SetState(Update::State::Idle);
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    g_app.Quit();   // the new one waits for our mutex, i.e. until the taskbar and tray are given back
}

void OnResult(Result* r) {
    switch (r->kind) {
    case Result::Checked: {
        SYSTEMTIME st;
        GetLocalTime(&st);
        wchar_t t[16];
        swprintf(t, 16, L"%02d:%02d", st.wHour, st.wMinute);
        g_lastChecked = t;
        if (g_state != Update::State::Checking) break;   // an install started meanwhile: leave it alone
        if (!r->ok) {
            // Keep an update we already found; a later check failing doesn't make it go away.
            if (g_avail.version.empty()) g_error = r->error;
            SetState(g_avail.version.empty() ? Update::State::Idle : Update::State::Available);
            break;
        }
        if (IsNewer(r->m.version)) {
            g_avail = r->m;
            SetState(Update::State::Available);
            MaybePrompt();
        } else {
            SetState(Update::State::UpToDate);
        }
        break;
    }
    case Result::Downloaded:
        if (!r->ok) { g_error = r->error; SetState(Update::State::Available); break; }
        SetState(Update::State::Installing);
        RunWorker([f = r->file, sha = r->m.sha256, exe = g_exe] { InstallWorker(f, sha, exe); });
        break;
    case Result::Installed:
        if (!r->ok) { g_error = r->error; SetState(Update::State::Available); break; }
        Restart();
        break;
    }
}

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TIMER:
        switch (wp) {
        case T_FIRST:
            KillTimer(h, T_FIRST);
            // Leftovers from the last update (Program Files may refuse: harmless, the next one replaces it).
            DeleteFileW(OldFile().c_str());
            DeleteFileW((ExeDir() + L"\\Skyline.new.exe").c_str());
            DeleteFileW(TempFile().c_str());
            SetTimer(h, T_REPEAT, kRepeat, nullptr);
            [[fallthrough]];
        case T_REPEAT:
            if (g_app.settings.updateCheck) Check();
            break;
        case T_PROMPT:
            MaybePrompt();
            break;
        }
        return 0;
    case WM_RESULT: {
        std::unique_ptr<Result> r((Result*)lp);
        OnResult(r.get());
        return 0;
    }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

} // namespace

// ---------------------------------------------------------------------------
bool Update::HandleCommandLine(int* exitCode) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return false;
    bool helper = false;
    if (argc >= 2 && !wcscmp(argv[1], L"--update-selftest")) {
        helper = true;
        *exitCode = kSelfTestCode;
    } else if (argc >= 4 && !wcscmp(argv[1], L"--apply-update")) {
        // Elevated: our own exe, started by InstallWorker when Skyline sits in Program Files.
        helper = true;
        std::string sha;
        for (const wchar_t* c = argv[3]; *c; ++c) sha += (char)towlower(*c);
        *exitCode = (int)Apply(argv[2], sha, ModulePath());
    } else if (argc >= 2 && !wcscmp(argv[1], L"--after-update")) {
        g_justUpdated = true;   // wWinMain waits for the old Skyline to let go of the single-instance mutex
    }
    LocalFree(argv);
    return helper;
}

void Update::Start() {
    g_exe = ModulePath();
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = g_app.hinst;
    wc.lpszClassName = kClass;
    RegisterClassW(&wc);
    g_wnd = CreateWindowExW(0, kClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, g_app.hinst, nullptr);
    SetTimer(g_wnd, T_FIRST, kFirstDelay, nullptr);
}

void Update::CheckNow() { Check(); }

void Update::Install() {
    if (g_avail.version.empty() || g_state == State::Downloading || g_state == State::Installing) return;
    g_error.clear();
    SetState(State::Downloading);
    RunWorker([m = g_avail] { DownloadWorker(m); });
}

Update::State Update::GetState() { return g_state; }
const std::wstring& Update::AvailableVersion() { return g_avail.version; }
std::wstring Update::Notes() { return g_norsk && !g_avail.notesNo.empty() ? g_avail.notesNo : g_avail.notes; }
const std::wstring& Update::Error() { return g_error; }
bool Update::JustUpdated() { return g_justUpdated; }
std::wstring Update::LastChecked() { return g_lastChecked; }

bool Update::HasKey() {
    for (size_t i = 8; i < sizeof kUpdateKey; ++i)
        if (kUpdateKey[i]) return true;
    return false;
}
