// Skyline — Copyright © 2026 Yen-Zen & Cyber. All rights reserved. See LICENSE.txt.
// File search via the Windows Search index (OLE DB provider "Search.CollatorDSO") — see filesearch.h.
#include "filesearch.h"
#include <oledb.h>
#include <shlobj.h>
#include <mutex>
#include <condition_variable>
#include <chrono>

namespace {

// From msdasc.h / oledb (defined here so nothing extra has to be linked)
const CLSID kCLSID_MSDAINITIALIZE = {0x2206CDB0, 0x19C1, 0x11D1, {0x89, 0xE0, 0x00, 0xC0, 0x4F, 0xD7, 0xA8, 0x29}};
const IID kIID_IDataInitialize = {0x2206CCB1, 0x19C1, 0x11D1, {0x89, 0xE0, 0x00, 0xC0, 0x4F, 0xD7, 0xA8, 0x29}};
const GUID kDBGUID_DEFAULT = {0xC8B521FB, 0x5CF3, 0x11CE, {0xAD, 0xE5, 0x00, 0xAA, 0x00, 0x44, 0x77, 0x3D}};

struct IDataInitializeMin : IUnknown {   // only the method we use (the first one)
    virtual HRESULT STDMETHODCALLTYPE GetDataSource(IUnknown* outer, DWORD ctx, LPWSTR init, REFIID riid, IUnknown** ds) = 0;
};

struct Request {
    HWND hwnd = nullptr;
    unsigned ticket = 0;
    std::wstring text;
    size_t max = 0;
    bool valid = false;
};

std::mutex g_lock;
std::condition_variable g_cv;
Request g_pending;
bool g_running = false, g_quit = false;
HANDLE g_thread = nullptr;

// One open connection to the index, kept while searches keep coming
struct Session {
    ComPtr<IDBInitialize> init;
    ComPtr<IDBCreateCommand> cmd;

    bool Open() {
        if (cmd) return true;
        ComPtr<IDataInitializeMin> di;
        if (FAILED(CoCreateInstance(kCLSID_MSDAINITIALIZE, nullptr, CLSCTX_INPROC_SERVER, kIID_IDataInitialize,
                                    reinterpret_cast<void**>(di.Put()))))
            return false;
        wchar_t conn[] = L"Provider=Search.CollatorDSO;Extended Properties=\"Application=Windows\"";
        ComPtr<IUnknown> ds;
        if (FAILED(di->GetDataSource(nullptr, CLSCTX_INPROC_SERVER, conn, IID_IDBInitialize, ds.Put()))) return false;
        if (FAILED(ds->QueryInterface(IID_IDBInitialize, reinterpret_cast<void**>(init.Put()))) || FAILED(init->Initialize())) return false;
        ComPtr<IDBCreateSession> cs;
        if (FAILED(init->QueryInterface(IID_IDBCreateSession, reinterpret_cast<void**>(cs.Put())))) return false;
        ComPtr<IUnknown> session;
        if (FAILED(cs->CreateSession(nullptr, IID_IDBCreateCommand, session.Put()))) return false;
        return SUCCEEDED(session->QueryInterface(IID_IDBCreateCommand, reinterpret_cast<void**>(cmd.Put())));
    }

    void Close() {
        cmd.Reset();
        if (init) init->Uninitialize();
        init.Reset();
    }
};

std::wstring ProfileFolder() {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &p))) { out = p; CoTaskMemFree(p); }
    return out;
}

// "rap 2026" -> "\"rap*\" AND \"2026*\"": every word, as the start of a word in the file name
std::wstring ContainsTerm(const std::wstring& text) {
    std::wstring out, word;
    auto flush = [&] {
        if (word.empty()) return;
        if (!out.empty()) out += L" AND ";
        out += L"\"" + word + L"*\"";
        word.clear();
    };
    for (wchar_t c : text) {
        if (iswspace(c)) { flush(); continue; }
        if (c == L'"' || c == L'\'' || c == L'*' || c == L'(' || c == L')' || c == L'[' || c == L']' || c < 32) continue;
        word += c;
    }
    flush();
    return out;
}

std::vector<AppEntry> Run(Session& s, const std::wstring& text, size_t max) {
    std::vector<AppEntry> out;
    std::wstring term = ContainsTerm(text);
    std::wstring scope = ProfileFolder();
    if (term.empty() || scope.empty() || !s.Open()) return out;
    for (auto& c : scope) if (c == L'\'') c = L' ';
    // Ask for a few more than needed: hidden/AppData items are dropped below.
    std::wstring sql = L"SELECT TOP " + std::to_wstring(max * 3 + 5) +
                       L" System.ItemPathDisplay, System.ItemNameDisplay, System.ItemType FROM SystemIndex WHERE SCOPE='file:" + scope +
                       L"' AND CONTAINS(System.FileName, '" + term + L"') ORDER BY System.Search.Rank DESC";
    ComPtr<ICommandText> ct;
    if (FAILED(s.cmd->CreateCommand(nullptr, IID_ICommandText, reinterpret_cast<IUnknown**>(ct.Put())))) { s.Close(); return out; }
    if (FAILED(ct->SetCommandText(kDBGUID_DEFAULT, sql.c_str()))) return out;
    ComPtr<IRowset> rows;
    if (FAILED(ct->Execute(nullptr, IID_IRowset, nullptr, nullptr, reinterpret_cast<IUnknown**>(rows.Put()))) || !rows) return out;
    ComPtr<IAccessor> acc;
    if (FAILED(rows->QueryInterface(IID_IAccessor, reinterpret_cast<void**>(acc.Put())))) return out;

    // Three text columns, each: status, length, then the text itself
    constexpr DBLENGTH kText = 1040 * sizeof(wchar_t);
    struct Col { DBSTATUS status; DBLENGTH len; wchar_t text[1040]; };
    DBBINDING b[3] = {};
    for (int i = 0; i < 3; ++i) {
        b[i].iOrdinal = i + 1;
        b[i].obStatus = i * sizeof(Col) + offsetof(Col, status);
        b[i].obLength = i * sizeof(Col) + offsetof(Col, len);
        b[i].obValue = i * sizeof(Col) + offsetof(Col, text);
        b[i].dwPart = DBPART_VALUE | DBPART_LENGTH | DBPART_STATUS;
        b[i].dwMemOwner = DBMEMOWNER_CLIENTOWNED;
        b[i].eParamIO = DBPARAMIO_NOTPARAM;
        b[i].cbMaxLen = kText;
        b[i].wType = DBTYPE_WSTR;
    }
    HACCESSOR h = 0;
    DBBINDSTATUS bs[3];
    if (FAILED(acc->CreateAccessor(DBACCESSOR_ROWDATA, 3, b, sizeof(Col) * 3, &h, bs))) return out;

    auto buf = std::make_unique<Col[]>(3);
    HROW handles[16];
    for (;;) {
        DBCOUNTITEM got = 0;
        HROW* p = handles;
        if (FAILED(rows->GetNextRows(DB_NULL_HCHAPTER, 0, 16, &got, &p)) || got == 0) break;
        for (DBCOUNTITEM r = 0; r < got && out.size() < max; ++r) {
            if (FAILED(rows->GetData(handles[r], h, buf.get()))) continue;
            auto str = [&](int i) { return buf[i].status == DBSTATUS_S_OK ? std::wstring(buf[i].text) : std::wstring(); };
            std::wstring path = str(0), name = str(1), type = ToLower(str(2));
            if (path.empty()) continue;
            std::wstring low = ToLower(path);
            if (low.find(L"\\appdata\\") != std::wstring::npos || low.find(L"\\.") != std::wstring::npos) continue;   // app junk, dot folders
            DWORD attr = GetFileAttributesW(path.c_str());
            if (attr == INVALID_FILE_ATTRIBUTES || (attr & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) continue;
            AppEntry a;
            a.file = true;
            a.folder = (attr & FILE_ATTRIBUTE_DIRECTORY) != 0 || type == L"directory";
            a.name = name.empty() ? FileNameOf(path) : name;
            a.target = path;
            a.key = L"file:" + low;
            out.push_back(std::move(a));
        }
        rows->ReleaseRows(got, handles, nullptr, nullptr, nullptr);
        if (out.size() >= max) break;
    }
    acc->ReleaseAccessor(h, nullptr);
    return out;
}

DWORD WINAPI Worker(LPVOID) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Session session;
    for (;;) {
        Request r;
        {
            std::unique_lock<std::mutex> l(g_lock);
            if (!g_cv.wait_for(l, std::chrono::seconds(60), [] { return g_pending.valid || g_quit; })) {
                g_running = false;   // a minute without searches: let go of everything
                break;
            }
            if (g_quit) { g_running = false; break; }
            r = g_pending;
            g_pending.valid = false;
        }
        auto* res = new std::vector<AppEntry>(Run(session, r.text, r.max));
        {
            std::lock_guard<std::mutex> l(g_lock);
            if (g_pending.valid && g_pending.hwnd == r.hwnd) { delete res; continue; }   // already typed more: skip this one
        }
        if (!IsWindow(r.hwnd) || !PostMessageW(r.hwnd, FileSearch::WM_SKY_FILES, r.ticket, reinterpret_cast<LPARAM>(res))) delete res;
    }
    session.Close();
    CoUninitialize();
    return 0;
}

} // namespace

namespace FileSearch {

void Query(HWND notify, unsigned ticket, const std::wstring& text, size_t max) {
    std::wstring t = text;
    while (!t.empty() && iswspace(t.back())) t.pop_back();
    if (t.size() < 2 || !max) return;
    std::lock_guard<std::mutex> l(g_lock);
    if (g_quit) return;
    g_pending = Request{notify, ticket, t, max, true};
    if (!g_running) {
        if (g_thread) { CloseHandle(g_thread); g_thread = nullptr; }   // the old one has finished (or is finishing) on its own
        g_thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
        g_running = g_thread != nullptr;
    }
    g_cv.notify_one();
}

void Shutdown() {
    {
        std::lock_guard<std::mutex> l(g_lock);
        g_quit = true;
        g_pending.valid = false;
    }
    g_cv.notify_all();
    if (g_thread) {
        WaitForSingleObject(g_thread, 3000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
}

} // namespace FileSearch
