/**
 * AugrixUpdater — Augrix EA Auto-Updater
 * Authenticates via API key, checks server version, and copies
 * the latest Augrix_EA.ex5 to all registered MT5 Experts paths.
 *
 * Modes:
 *   Interactive (no args):  TUI menu for manual setup + update
 *   --watch:                Silent flag-file watcher (run via Scheduled Task)
 *   --install-task:         Register Windows Scheduled Task (5-min cadence)
 *   --remove-task:          Remove the Scheduled Task
 *
 * Build:
 *   cmake -B build -A x64
 *   cmake --build build --config Release
 */

#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <wincred.h>
#include <shlwapi.h>
#include <tlhelp32.h>
#include <taskschd.h>
#include <comdef.h>
#include <conio.h>
#include <io.h>
#include <fcntl.h>

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <algorithm>
#include <chrono>
#include <thread>

// Libs linked via CMakeLists.txt: winhttp, ws2_32, shlwapi, advapi32, taskschd, ole32, oleaut32
#pragma comment(lib, "taskschd.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

// MinGW wincred.h may be missing CRED_PERSIST_CURRENT_USER (value 1)
#ifndef CRED_PERSIST_CURRENT_USER
#define CRED_PERSIST_CURRENT_USER 1
#endif

namespace fs = std::filesystem;

// ─── Constants ───────────────────────────────────────────────────────────────

static const std::wstring SERVER_HOST  = L"augrix.io";
static const INTERNET_PORT SERVER_PORT = INTERNET_DEFAULT_HTTPS_PORT;
static const std::wstring VERSION_PATH = L"/api/version";
static const std::wstring DOWNLOAD_PATH = L"/api/download/ea";
static const std::wstring ALLOWED_PATH  = L"/api/updater/allowed";
static const std::wstring EVENT_PATH    = L"/api/updater/event";
static const std::string  EA_FILENAME  = "Augrix_EA.ex5";
static const std::string  FLAG_FILENAME = "augrix_update.json";
static const std::wstring TASK_NAME    = L"AugrixUpdater";

// Named mutex — single instance guard for --watch
static const wchar_t* MUTEX_NAME = L"Global\\AugrixUpdaterWatchMutex";

// ─── ANSI colors ─────────────────────────────────────────────────────────────

namespace Color {
    const std::string Reset   = "\033[0m";
    const std::string Bold    = "\033[1m";
    const std::string Dim     = "\033[2m";
    const std::string Green   = "\033[32m";
    const std::string Yellow  = "\033[33m";
    const std::string Red     = "\033[31m";
    const std::string Cyan    = "\033[36m";
    const std::string White   = "\033[97m";
    const std::string BoldGreen  = "\033[1;32m";
    const std::string BoldYellow = "\033[1;33m";
    const std::string BoldRed    = "\033[1;31m";
    const std::string BoldCyan   = "\033[1;36m";
    const std::string BoldWhite  = "\033[1;97m";
}

// ─── Helpers ─────────────────────────────────────────────────────────────────

static void enableAnsi() {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut == INVALID_HANDLE_VALUE) return;
    DWORD mode = 0;
    GetConsoleMode(hOut, &mode);
    SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
}

static void clearScreen() {
    system("cls");
}

static std::string narrow(const std::wstring& ws) {
    if (ws.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.data(), (int)ws.size(), nullptr, 0, nullptr, nullptr);
    std::string s(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.data(), (int)ws.size(), s.data(), len, nullptr, nullptr);
    return s;
}

static std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring ws(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), ws.data(), len);
    return ws;
}


static void pressAnyKey() {
    std::cout << "\n  Press any key to continue...";
    FlushConsoleInputBuffer(GetStdHandle(STD_INPUT_HANDLE));
    _getch();
}

static void printSeparator(char ch = '-', int width = 60) {
    std::cout << Color::Dim;
    for (int i = 0; i < width; ++i) std::cout << ch;
    std::cout << Color::Reset << "\n";
}

static void printHeader() {
    std::cout << Color::BoldCyan
        << R"(
 ░▒▓██████▓▒░░▒▓█▓▒░░▒▓█▓▒░░▒▓██████▓▒░░▒▓███████▓▒░░▒▓█▓▒░▒▓█▓▒░░▒▓█▓▒░
░▒▓█▓▒░░▒▓█▓▒░▒▓█▓▒░░▒▓█▓▒░▒▓█▓▒░░▒▓█▓▒░▒▓█▓▒░░▒▓█▓▒░▒▓█▓▒░▒▓█▓▒░░▒▓█▓▒░
░▒▓█▓▒░░▒▓█▓▒░▒▓█▓▒░░▒▓█▓▒░▒▓█▓▒░      ░▒▓█▓▒░░▒▓█▓▒░▒▓█▓▒░▒▓█▓▒░░▒▓█▓▒░
░▒▓████████▓▒░▒▓█▓▒░░▒▓█▓▒░▒▓█▓▒▒▓███▓▒░▒▓███████▓▒░░▒▓█▓▒░░▒▓██████▓▒░
░▒▓█▓▒░░▒▓█▓▒░▒▓█▓▒░░▒▓█▓▒░▒▓█▓▒░░▒▓█▓▒░▒▓█▓▒░░▒▓█▓▒░▒▓█▓▒░▒▓█▓▒░░▒▓█▓▒░
░▒▓█▓▒░░▒▓█▓▒░▒▓█▓▒░░▒▓█▓▒░▒▓█▓▒░░▒▓█▓▒░▒▓█▓▒░░▒▓█▓▒░▒▓█▓▒░▒▓█▓▒░░▒▓█▓▒░
░▒▓█▓▒░░▒▓█▓▒░░▒▓██████▓▒░ ░▒▓██████▓▒░░▒▓█▓▒░░▒▓█▓▒░▒▓█▓▒░▒▓█▓▒░░▒▓█▓▒░


)" << Color::Reset << "\n";
}

// ─── Logging (silent for --watch, verbose for interactive) ──────────────────

static bool g_silent = false;

static void logInfo(const std::string& msg) {
    if (!g_silent) std::cout << "  " << Color::Green << "[INFO] " << Color::Reset << msg << "\n";
}
static void logWarn(const std::string& msg) {
    if (!g_silent) std::cout << "  " << Color::Yellow << "[WARN] " << Color::Reset << msg << "\n";
}
static void logError(const std::string& msg) {
    std::cerr << "  " << Color::BoldRed << "[ERROR] " << Color::Reset << msg << "\n";
}

// ─── Storage: Credential Manager (API key) + Registry (paths) ────────────────

static const wchar_t* CRED_TARGET  = L"AugrixUpdater/ApiKey";
static const wchar_t* REG_KEY_PATH = L"Software\\Augrix\\Updater";
static const wchar_t* REG_VAL_NAME = L"Paths";

static std::string loadApiKey() {
    PCREDENTIALW cred = nullptr;
    if (!CredReadW(CRED_TARGET, CRED_TYPE_GENERIC, 0, &cred)) return {};
    std::string key(reinterpret_cast<char*>(cred->CredentialBlob), cred->CredentialBlobSize);
    CredFree(cred);
    return key;
}

static void saveApiKey(const std::string& key) {
    CREDENTIALW cred  = {};
    cred.Type         = CRED_TYPE_GENERIC;
    cred.TargetName   = const_cast<LPWSTR>(CRED_TARGET);
    cred.CredentialBlob     = reinterpret_cast<LPBYTE>(const_cast<char*>(key.data()));
    cred.CredentialBlobSize = static_cast<DWORD>(key.size());
    cred.Persist      = CRED_PERSIST_CURRENT_USER;
    CredWriteW(&cred, 0);
}

static std::vector<std::string> loadPaths() {
    std::vector<std::string> paths;
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_KEY_PATH, 0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return paths;
    DWORD type = 0, size = 0;
    if (RegQueryValueExW(hKey, REG_VAL_NAME, nullptr, &type, nullptr, &size) == ERROR_SUCCESS
        && type == REG_MULTI_SZ && size > sizeof(wchar_t)) {
        std::vector<wchar_t> buf(size / sizeof(wchar_t));
        RegQueryValueExW(hKey, REG_VAL_NAME, nullptr, &type,
                         reinterpret_cast<LPBYTE>(buf.data()), &size);
        for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1)
            paths.push_back(narrow(p));
    }
    RegCloseKey(hKey);
    return paths;
}

static void savePaths(const std::vector<std::string>& paths) {
    HKEY hKey;
    RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY_PATH, 0, nullptr,
                    REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKey, nullptr);
    // REG_MULTI_SZ: null-separated wide strings, double-null terminated
    std::vector<wchar_t> buf;
    for (auto& p : paths) {
        std::wstring ws = widen(p);
        buf.insert(buf.end(), ws.begin(), ws.end());
        buf.push_back(L'\0');
    }
    buf.push_back(L'\0');
    RegSetValueExW(hKey, REG_VAL_NAME, 0, REG_MULTI_SZ,
                   reinterpret_cast<const BYTE*>(buf.data()),
                   static_cast<DWORD>(buf.size() * sizeof(wchar_t)));
    RegCloseKey(hKey);
}

struct Config {
    std::string              apiKey;
    std::vector<std::string> paths;

    static Config load() {
        Config cfg;
        cfg.apiKey = loadApiKey();
        cfg.paths  = loadPaths();
        return cfg;
    }

    void save()      const { saveApiKey(apiKey); ::savePaths(paths); }
    void saveKey()   const { saveApiKey(apiKey); }
    void savePaths() const { ::savePaths(paths); }

    bool hasApiKey() const { return !apiKey.empty(); }
};

// ─── WinHTTP wrapper ─────────────────────────────────────────────────────────

struct HttpResponse {
    DWORD statusCode = 0;
    std::string body;
    std::string contentDisposition;
    std::string signatureHeader; // X-Signature-Ed25519
    std::string versionHeader;   // X-Build-Version
};

class Http {
public:
    // GET request; optionally with auth header. Returns response.
    static HttpResponse get(
        const std::wstring& host,
        INTERNET_PORT port,
        const std::wstring& path,
        const std::string& bearerToken = "",
        std::function<void(size_t, size_t)> progressCb = nullptr
    ) {
        HttpResponse resp;

        HINTERNET hSession = WinHttpOpen(
            L"AugrixUpdater/2.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession) throw std::runtime_error("WinHttpOpen failed");

        HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), port, 0);
        if (!hConnect) { WinHttpCloseHandle(hSession); throw std::runtime_error("WinHttpConnect failed"); }

        HINTERNET hRequest = WinHttpOpenRequest(
            hConnect, L"GET", path.c_str(),
            nullptr, WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            WINHTTP_FLAG_SECURE);
        if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); throw std::runtime_error("WinHttpOpenRequest failed"); }

        if (!bearerToken.empty()) {
            std::wstring authHeader = L"Authorization: Bearer " + widen(bearerToken);
            WinHttpAddRequestHeaders(hRequest, authHeader.c_str(), (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD);
        }

        if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
            WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
            throw std::runtime_error("WinHttpSendRequest failed");
        }

        if (!WinHttpReceiveResponse(hRequest, nullptr)) {
            WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
            throw std::runtime_error("WinHttpReceiveResponse failed");
        }

        // Status code
        DWORD statusCode = 0;
        DWORD statusSize = sizeof(statusCode);
        WinHttpQueryHeaders(hRequest,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);
        resp.statusCode = statusCode;

        // Content-Disposition header
        wchar_t cdBuf[512] = {};
        DWORD cdSize = sizeof(cdBuf);
        if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_CONTENT_DISPOSITION,
                WINHTTP_HEADER_NAME_BY_INDEX, cdBuf, &cdSize, WINHTTP_NO_HEADER_INDEX)) {
            resp.contentDisposition = narrow(cdBuf);
        }

        // X-Signature-Ed25519
        wchar_t sigBuf[256] = {};
        DWORD sigSize = sizeof(sigBuf);
        if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_CUSTOM,
                L"X-Signature-Ed25519", sigBuf, &sigSize, WINHTTP_NO_HEADER_INDEX)) {
            resp.signatureHeader = narrow(sigBuf);
        }

        // X-Build-Version
        wchar_t verBuf[128] = {};
        DWORD verSize = sizeof(verBuf);
        if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_CUSTOM,
                L"X-Build-Version", verBuf, &verSize, WINHTTP_NO_HEADER_INDEX)) {
            resp.versionHeader = narrow(verBuf);
        }

        // Content-Length for progress
        size_t totalBytes = 0;
        wchar_t clBuf[64] = {};
        DWORD clSize = sizeof(clBuf);
        if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_CONTENT_LENGTH,
                WINHTTP_HEADER_NAME_BY_INDEX, clBuf, &clSize, WINHTTP_NO_HEADER_INDEX)) {
            totalBytes = (size_t)_wtoi64(clBuf);
        }

        // Read body
        size_t received = 0;
        DWORD bytesAvailable = 0;
        while (WinHttpQueryDataAvailable(hRequest, &bytesAvailable) && bytesAvailable > 0) {
            std::string chunk(bytesAvailable, '\0');
            DWORD bytesRead = 0;
            WinHttpReadData(hRequest, chunk.data(), bytesAvailable, &bytesRead);
            resp.body.append(chunk.data(), bytesRead);
            received += bytesRead;
            if (progressCb) progressCb(received, totalBytes);
        }

        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return resp;
    }

    // POST request with JSON body. Returns response.
    static HttpResponse post(
        const std::wstring& host,
        INTERNET_PORT port,
        const std::wstring& path,
        const std::string& bearerToken,
        const std::string& jsonBody
    ) {
        HttpResponse resp;

        HINTERNET hSession = WinHttpOpen(
            L"AugrixUpdater/2.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession) throw std::runtime_error("WinHttpOpen failed");

        HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), port, 0);
        if (!hConnect) { WinHttpCloseHandle(hSession); throw std::runtime_error("WinHttpConnect failed"); }

        HINTERNET hRequest = WinHttpOpenRequest(
            hConnect, L"POST", path.c_str(),
            nullptr, WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            WINHTTP_FLAG_SECURE);
        if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); throw std::runtime_error("WinHttpOpenRequest failed"); }

        std::wstring headers = L"Content-Type: application/json\r\n";
        if (!bearerToken.empty()) {
            headers += L"Authorization: Bearer " + widen(bearerToken) + L"\r\n";
        }
        WinHttpAddRequestHeaders(hRequest, headers.c_str(), (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD);

        if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                (LPVOID)jsonBody.data(), (DWORD)jsonBody.size(),
                                (DWORD)jsonBody.size(), 0)) {
            WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
            throw std::runtime_error("WinHttpSendRequest failed");
        }

        if (!WinHttpReceiveResponse(hRequest, nullptr)) {
            WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
            throw std::runtime_error("WinHttpReceiveResponse failed");
        }

        DWORD sc = 0; DWORD ss = sizeof(sc);
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &sc, &ss, WINHTTP_NO_HEADER_INDEX);
        resp.statusCode = sc;

        DWORD bytesAvailable = 0;
        while (WinHttpQueryDataAvailable(hRequest, &bytesAvailable) && bytesAvailable > 0) {
            std::string chunk(bytesAvailable, '\0');
            DWORD bytesRead = 0;
            WinHttpReadData(hRequest, chunk.data(), bytesAvailable, &bytesRead);
            resp.body.append(chunk.data(), bytesRead);
        }

        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return resp;
    }
};

// ─── MT5 Process Control ────────────────────────────────────────────────────

struct MT5Process {
    DWORD pid;
    std::string exePath;
};

// Find all terminal64.exe processes
static std::vector<MT5Process> findMT5Processes() {
    std::vector<MT5Process> procs;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return procs;

    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            std::wstring name(pe.szExeFile);
            // Case-insensitive compare
            std::transform(name.begin(), name.end(), name.begin(), ::towlower);
            if (name == L"terminal64.exe" || name == L"terminal.exe") {
                MT5Process mp;
                mp.pid = pe.th32ProcessID;
                // Get full path
                HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
                if (hProc) {
                    wchar_t path[MAX_PATH] = {};
                    DWORD pathSize = MAX_PATH;
                    if (QueryFullProcessImageNameW(hProc, 0, path, &pathSize)) {
                        mp.exePath = narrow(path);
                    }
                    CloseHandle(hProc);
                }
                procs.push_back(mp);
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return procs;
}

// Find MT5 PID by matching terminalDataPath from the flag file.
// The terminalDataPath contains the MT5 data directory which includes
// the terminal hash — we match this against running terminal64 paths.
static DWORD findMT5PidByDataPath(const std::string& terminalDataPath) {
    auto procs = findMT5Processes();
    if (procs.empty()) return 0;

    // terminalDataPath looks like:
    //   C:\Users\You\AppData\Roaming\MetaQuotes\Terminal\<HASH>
    // exePath looks like:
    //   C:\Program Files\MetaTrader 5\terminal64.exe
    //   or any custom install path
    // We can't directly match them. Instead, check if the terminal's
    // data folder config matches. For now, return the first MT5 if only
    // one is running; if multiple, try to match by checking which
    // terminal has our Experts folder with the EA.

    // Extract the hash from terminalDataPath
    fs::path dp(terminalDataPath);
    std::string hash = dp.filename().string(); // The <HASH> part

    // If only one MT5 running, use it
    if (procs.size() == 1) return procs[0].pid;

    // Multiple MT5: check which one has augrix_update.json in its Files dir
    for (auto& p : procs) {
        // Try the terminalDataPath directly
        fs::path flagPath = fs::path(terminalDataPath) / "MQL5" / "Files" / FLAG_FILENAME;
        if (fs::exists(flagPath)) return p.pid;
    }

    // Fallback: return first
    return procs.empty() ? 0 : procs[0].pid;
}

// Gracefully close MT5 via WM_CLOSE to all windows of the process
static bool closeMT5Process(DWORD pid, int timeoutMs = 15000) {
    struct EnumData {
        DWORD pid;
        std::vector<HWND> windows;
    } data;
    data.pid = pid;

    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        auto* d = reinterpret_cast<EnumData*>(lParam);
        DWORD wndPid = 0;
        GetWindowThreadProcessId(hwnd, &wndPid);
        if (wndPid == d->pid && IsWindowVisible(hwnd)) {
            d->windows.push_back(hwnd);
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&data));

    for (HWND hwnd : data.windows) {
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
    }

    // Wait for process to exit
    HANDLE hProc = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) return true; // Already gone

    DWORD result = WaitForSingleObject(hProc, timeoutMs);
    CloseHandle(hProc);
    return (result == WAIT_OBJECT_0);
}

// Check if a process is still alive
static bool isProcessAlive(DWORD pid) {
    HANDLE hProc = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) return false;
    DWORD exitCode = 0;
    GetExitCodeProcess(hProc, &exitCode);
    CloseHandle(hProc);
    return (exitCode == STILL_ACTIVE);
}

// Start MT5 from its executable path
static bool startMT5(const std::string& exePath) {
    if (exePath.empty()) return false;

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    std::wstring cmdLine = widen(exePath);

    BOOL ok = CreateProcessW(
        nullptr,
        &cmdLine[0],
        nullptr, nullptr, FALSE,
        0, nullptr, nullptr,
        &si, &pi);

    if (ok) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    return ok != 0;
}

// ─── Flag file parsing ─────────────────────────────────────────────────────

struct UpdateFlag {
    std::string fromVersion;
    std::string toVersion;
    std::string terminalDataPath;
    std::string botId;
    long long magic = 0;
    long long timestamp = 0;
    bool valid = false;
};

// Minimal JSON parser — flag file is a flat JSON object with string/number values.
// No external dependency needed for this simple structure.
static std::string extractJsonString(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\":\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return {};
    pos += search.size();
    auto end = json.find('"', pos);
    if (end == std::string::npos) return {};
    return json.substr(pos, end - pos);
}

static long long extractJsonNumber(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\":";
    auto pos = json.find(search);
    if (pos == std::string::npos) return 0;
    pos += search.size();
    // Skip whitespace
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    std::string num;
    while (pos < json.size() && (isdigit(json[pos]) || json[pos] == '-')) {
        num += json[pos++];
    }
    try { return std::stoll(num); } catch (...) { return 0; }
}

static UpdateFlag parseUpdateFlag(const std::string& content) {
    UpdateFlag f;
    f.fromVersion = extractJsonString(content, "fromVersion");
    f.toVersion = extractJsonString(content, "toVersion");
    f.terminalDataPath = extractJsonString(content, "terminalDataPath");
    f.botId = extractJsonString(content, "botId");
    f.magic = extractJsonNumber(content, "magic");
    f.timestamp = extractJsonNumber(content, "timestamp");
    f.valid = !f.fromVersion.empty() && !f.toVersion.empty();
    return f;
}

// ─── Scan for flag files across registered paths + known MT5 data dirs ──────

struct FlagHit {
    UpdateFlag flag;
    fs::path flagPath;          // Full path to the flag file
    fs::path expertsDir;        // The Experts dir to install to
    std::string mt5ExePath;     // Resolved path to terminal64.exe (for restart)
};

static std::vector<FlagHit> scanForFlags(const std::vector<std::string>& registeredPaths) {
    std::vector<FlagHit> hits;
    std::vector<fs::path> searchDirs;

    // Search in registered Experts paths' sibling Files dir
    for (auto& p : registeredPaths) {
        // Registered path is .../MQL5/Experts — Files is .../MQL5/Files
        fs::path experts(p);
        fs::path filesDir = experts.parent_path() / "Files";
        if (fs::exists(filesDir)) {
            searchDirs.push_back(filesDir);
        }
    }

    // Also scan via AppData MetaQuotes paths (discover unregistered terminals)
    try {
        const char* appdata = std::getenv("APPDATA");
        if (appdata) {
            fs::path mqRoot = fs::path(appdata) / "MetaQuotes" / "Terminal";
            if (fs::exists(mqRoot)) {
                for (auto& entry : fs::directory_iterator(mqRoot)) {
                    if (!entry.is_directory()) continue;
                    fs::path filesDir = entry.path() / "MQL5" / "Files";
                    if (fs::exists(filesDir)) {
                        // Check if already in searchDirs
                        bool dupe = false;
                        for (auto& sd : searchDirs) {
                            if (fs::equivalent(sd, filesDir)) { dupe = true; break; }
                        }
                        if (!dupe) searchDirs.push_back(filesDir);
                    }
                }
            }
        }
    } catch (...) {}

    for (auto& dir : searchDirs) {
        fs::path flagPath = dir / FLAG_FILENAME;
        if (!fs::exists(flagPath)) continue;

        try {
            std::ifstream ifs(flagPath, std::ios::binary);
            std::string content((std::istreambuf_iterator<char>(ifs)),
                                 std::istreambuf_iterator<char>());
            ifs.close();

            auto flag = parseUpdateFlag(content);
            if (!flag.valid) {
                logWarn("Invalid flag file at " + flagPath.string());
                continue;
            }

            FlagHit hit;
            hit.flag = flag;
            hit.flagPath = flagPath;
            // Files dir is .../MQL5/Files → Experts is .../MQL5/Experts
            hit.expertsDir = dir.parent_path() / "Experts";

            // Resolve MT5 exe path from terminalDataPath
            // terminalDataPath → parent is Terminal dir → check for terminal64.exe
            // But MT5 exe might be in a different location (Program Files).
            // We'll store the data path and resolve the exe at close time.
            if (!flag.terminalDataPath.empty()) {
                // Try common locations
                fs::path dataPath(flag.terminalDataPath);
                // Check if terminal64.exe is alongside the data path (portable install)
                fs::path alongside = dataPath / "terminal64.exe";
                if (fs::exists(alongside)) {
                    hit.mt5ExePath = alongside.string();
                }
                // If not found, we'll rely on the running process list
            }

            hits.push_back(hit);
        } catch (std::exception& e) {
            logError("Error reading flag " + flagPath.string() + ": " + e.what());
        }
    }

    return hits;
}

// ─── Event reporting ────────────────────────────────────────────────────────

static void reportEvent(const std::string& apiKey, const std::string& fromVersion,
                         const std::string& toVersion, const std::string& status,
                         const std::string& error = "") {
    try {
        std::ostringstream json;
        json << "{\"fromVersion\":\"" << fromVersion
             << "\",\"toVersion\":\"" << toVersion
             << "\",\"status\":\"" << status << "\"";
        if (!error.empty()) {
            // Escape double quotes in error message
            std::string escaped = error;
            size_t pos = 0;
            while ((pos = escaped.find('"', pos)) != std::string::npos) {
                escaped.replace(pos, 1, "\\\"");
                pos += 2;
            }
            json << ",\"error\":\"" << escaped << "\"";
        }
        json << "}";

        Http::post(SERVER_HOST, SERVER_PORT, EVENT_PATH, apiKey, json.str());
    } catch (...) {
        // Fire and forget — don't block update on reporting failure
    }
}

// ─── Watch mode ─────────────────────────────────────────────────────────────

static int runWatch(Config& cfg) {
    // Single instance guard
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, MUTEX_NAME);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // Another instance is already running
        if (hMutex) CloseHandle(hMutex);
        return 0; // Silent exit — not an error
    }

    if (!cfg.hasApiKey()) {
        logError("No API key configured. Run AugrixUpdater.exe to set up.");
        if (hMutex) { ReleaseMutex(hMutex); CloseHandle(hMutex); }
        return 1;
    }

    // Check if updater is allowed for this user
    try {
        auto resp = Http::get(SERVER_HOST, SERVER_PORT, ALLOWED_PATH, cfg.apiKey);
        if (resp.statusCode == 200) {
            // Check response for "allowed":false
            if (resp.body.find("\"allowed\":false") != std::string::npos) {
                logInfo("Update not allowed by server (kill-switch active or user excluded).");
                if (hMutex) { ReleaseMutex(hMutex); CloseHandle(hMutex); }
                return 0;
            }
        }
    } catch (...) {
        // Server unreachable — continue anyway, EA wrote the flag for a reason
    }

    // Scan for flag files
    auto hits = scanForFlags(cfg.paths);
    if (hits.empty()) {
        // No flags — nothing to do. Normal exit.
        if (hMutex) { ReleaseMutex(hMutex); CloseHandle(hMutex); }
        return 0;
    }

    logInfo("Found " + std::to_string(hits.size()) + " update flag(s).");

    for (auto& hit : hits) {
        logInfo("Processing: " + hit.flag.fromVersion + " -> " + hit.flag.toVersion);

        // Report started
        reportEvent(cfg.apiKey, hit.flag.fromVersion, hit.flag.toVersion, "started");

        // 1. Download the new EA binary
        std::string eaBytes;
        std::string signature;
        try {
            auto resp = Http::get(SERVER_HOST, SERVER_PORT, DOWNLOAD_PATH, cfg.apiKey);
            if (resp.statusCode != 200) {
                std::string err = "Download failed (HTTP " + std::to_string(resp.statusCode) + ")";
                logError(err);
                reportEvent(cfg.apiKey, hit.flag.fromVersion, hit.flag.toVersion, "failed", err);
                // Remove flag so we don't retry forever
                try { fs::remove(hit.flagPath); } catch (...) {}
                continue;
            }
            eaBytes = std::move(resp.body);
            signature = resp.signatureHeader;
        } catch (std::exception& e) {
            std::string err = std::string("Download error: ") + e.what();
            logError(err);
            reportEvent(cfg.apiKey, hit.flag.fromVersion, hit.flag.toVersion, "failed", err);
            try { fs::remove(hit.flagPath); } catch (...) {}
            continue;
        }

        if (eaBytes.empty()) {
            logError("Downloaded empty binary");
            reportEvent(cfg.apiKey, hit.flag.fromVersion, hit.flag.toVersion, "failed", "empty binary");
            try { fs::remove(hit.flagPath); } catch (...) {}
            continue;
        }

        logInfo("Downloaded " + std::to_string(eaBytes.size() / 1024) + " KB");

        // 2. Find and close the MT5 process
        DWORD mt5Pid = findMT5PidByDataPath(hit.flag.terminalDataPath);
        std::string mt5ExePath = hit.mt5ExePath;

        if (mt5Pid != 0) {
            // Grab the exe path before closing (for restart)
            if (mt5ExePath.empty()) {
                HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, mt5Pid);
                if (hProc) {
                    wchar_t path[MAX_PATH] = {};
                    DWORD pathSize = MAX_PATH;
                    if (QueryFullProcessImageNameW(hProc, 0, path, &pathSize)) {
                        mt5ExePath = narrow(path);
                    }
                    CloseHandle(hProc);
                }
            }

            logInfo("Closing MT5 (PID " + std::to_string(mt5Pid) + ")...");
            if (!closeMT5Process(mt5Pid, 15000)) {
                // MT5 didn't close gracefully. Try one more time.
                logWarn("MT5 did not close gracefully, retrying...");
                std::this_thread::sleep_for(std::chrono::seconds(2));
                if (isProcessAlive(mt5Pid)) {
                    // Force kill as last resort
                    HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, mt5Pid);
                    if (hProc) {
                        TerminateProcess(hProc, 1);
                        CloseHandle(hProc);
                        std::this_thread::sleep_for(std::chrono::seconds(1));
                    }
                }
            }
            logInfo("MT5 closed.");
        } else {
            logInfo("MT5 not running — EA already unloaded itself.");
        }

        // 3. Wait a moment for file handles to release
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        // 4. Backup old + install new binary
        fs::path dest = hit.expertsDir / EA_FILENAME;
        fs::path backup = hit.expertsDir / (EA_FILENAME + ".bak");
        fs::path tmp = hit.expertsDir / (EA_FILENAME + ".tmp");

        bool installed = false;
        try {
            // Backup existing
            if (fs::exists(dest)) {
                if (fs::exists(backup)) fs::remove(backup);
                fs::copy_file(dest, backup);
            }

            // Write to temp then atomic rename
            {
                std::ofstream out(tmp, std::ios::binary);
                out.write(eaBytes.data(), (std::streamsize)eaBytes.size());
                out.close();
            }

            // Verify temp file size matches
            if (fs::file_size(tmp) != eaBytes.size()) {
                throw std::runtime_error("Written file size mismatch");
            }

            // Swap: delete old, rename temp
            if (fs::exists(dest)) fs::remove(dest);
            fs::rename(tmp, dest);
            installed = true;
            logInfo("Binary installed to " + dest.string());
        } catch (std::exception& e) {
            std::string err = std::string("Install failed: ") + e.what();
            logError(err);
            reportEvent(cfg.apiKey, hit.flag.fromVersion, hit.flag.toVersion, "failed", err);

            // Rollback from backup
            if (fs::exists(backup) && !fs::exists(dest)) {
                try {
                    fs::rename(backup, dest);
                    logInfo("Rolled back to previous version.");
                    reportEvent(cfg.apiKey, hit.flag.fromVersion, hit.flag.toVersion, "rolled_back", err);
                } catch (...) {}
            }

            // Clean up temp
            try { if (fs::exists(tmp)) fs::remove(tmp); } catch (...) {}
            try { fs::remove(hit.flagPath); } catch (...) {}
            continue;
        }

        // 5. Remove the flag file
        try { fs::remove(hit.flagPath); } catch (...) {}

        // 6. Restart MT5
        if (!mt5ExePath.empty()) {
            logInfo("Restarting MT5...");
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (startMT5(mt5ExePath)) {
                logInfo("MT5 started. EA will reload automatically.");
                reportEvent(cfg.apiKey, hit.flag.fromVersion, hit.flag.toVersion, "completed");
            } else {
                logWarn("Could not restart MT5. Please start it manually.");
                reportEvent(cfg.apiKey, hit.flag.fromVersion, hit.flag.toVersion, "completed",
                           "MT5 restart failed — user must start manually");
            }
        } else {
            logInfo("MT5 exe path unknown — please restart MT5 manually.");
            reportEvent(cfg.apiKey, hit.flag.fromVersion, hit.flag.toVersion, "completed",
                       "MT5 path unknown — user must start manually");
        }

        // Clean up backup after successful update
        // Keep it for one cycle in case user needs to manually roll back
    }

    if (hMutex) { ReleaseMutex(hMutex); CloseHandle(hMutex); }
    return 0;
}

// ─── Scheduled Task management ──────────────────────────────────────────────

static bool installScheduledTask() {
    // Get path to our own executable
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    // Use schtasks.exe for simplicity (COM API is heavy)
    std::wstring cmd = L"schtasks /Create /F /TN \"" + std::wstring(TASK_NAME)
        + L"\" /TR \"\\\"" + exePath + L"\\\" --watch\""
        + L" /SC MINUTE /MO 5"       // Every 5 minutes
        + L" /RL HIGHEST"             // Run with highest privileges (for process control)
        + L" /IT";                    // Only when user is logged in (interactive token)

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};

    std::wstring fullCmd = L"cmd.exe /C " + cmd;

    BOOL ok = CreateProcessW(
        nullptr, &fullCmd[0],
        nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr,
        &si, &pi);

    if (ok) {
        WaitForSingleObject(pi.hProcess, 10000);
        DWORD exitCode = 1;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return exitCode == 0;
    }
    return false;
}

static bool removeScheduledTask() {
    std::wstring cmd = L"schtasks /Delete /F /TN \"" + std::wstring(TASK_NAME) + L"\"";

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};

    std::wstring fullCmd = L"cmd.exe /C " + cmd;

    BOOL ok = CreateProcessW(
        nullptr, &fullCmd[0],
        nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr,
        &si, &pi);

    if (ok) {
        WaitForSingleObject(pi.hProcess, 10000);
        DWORD exitCode = 1;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return exitCode == 0;
    }
    return false;
}

// ─── Screens (Interactive Mode) ─────────────────────────────────────────────

static void printMainHeader(const Config& cfg) {
    clearScreen();
    printHeader();
    printSeparator('=');

    std::cout << "  " << Color::BoldWhite << "Augrix EA Autoupdater v2.0" << Color::Reset;

    std::cout << "  |  API Key: ";
    if (cfg.hasApiKey()) {
        std::cout << Color::BoldGreen << "configured" << Color::Reset;
    } else {
        std::cout << Color::BoldRed << "NOT SET" << Color::Reset;
    }

    std::cout << "  |  Paths: "
              << Color::BoldCyan << cfg.paths.size() << Color::Reset << "\n";
    printSeparator('=');
    std::cout << "\n";
}

// ─── Menu: Update ────────────────────────────────────────────────────────────

static void menuUpdate(Config& cfg) {
    clearScreen();
    printHeader();
    printSeparator();
    std::cout << Color::BoldWhite << "  Update the Bots\n" << Color::Reset;
    printSeparator();
    std::cout << "\n";

    if (!cfg.hasApiKey()) {
        std::cout << Color::BoldRed << "  [!] No API key set. Please set your API key first (option 4).\n" << Color::Reset;
        pressAnyKey();
        return;
    }
    if (cfg.paths.empty()) {
        std::cout << Color::BoldYellow << "  [!] No paths configured. Please add a path first (option 2).\n" << Color::Reset;
        pressAnyKey();
        return;
    }

    // Confirm
    std::cout << "  Will download and install the latest Augrix EA to "
              << Color::BoldCyan << cfg.paths.size() << Color::Reset << " path(s).\n\n";
    std::cout << "  Proceed? [y/N] ";
    std::cout.flush();

    char yn = '\0';
    std::string line;
    std::getline(std::cin, line);
    if (!line.empty()) yn = (char)tolower(line[0]);
    if (yn != 'y') {
        std::cout << "\n  " << Color::Yellow << "Cancelled." << Color::Reset << "\n";
        pressAnyKey();
        return;
    }
    std::cout << "\n";

    // Step 3: download
    std::cout << "  Downloading Augrix_EA.ex5...";
    std::cout.flush();

    std::string eaBytes;
    try {
        size_t lastPct = 0;
        auto resp = Http::get(SERVER_HOST, SERVER_PORT, DOWNLOAD_PATH, cfg.apiKey,
            [&](size_t recv, size_t total) {
                if (total > 0) {
                    size_t pct = recv * 100 / total;
                    if (pct != lastPct && pct % 10 == 0) {
                        std::cout << " " << pct << "%";
                        std::cout.flush();
                        lastPct = pct;
                    }
                }
            });

        if (resp.statusCode == 401 || resp.statusCode == 403) {
            std::cout << "\n\n  " << Color::BoldRed
                      << "[!] Authentication failed (HTTP " << resp.statusCode << ").\n"
                      << "  Make sure your API key is valid and your license is active.\n"
                      << Color::Reset;
            pressAnyKey();
            return;
        }
        if (resp.statusCode != 200) {
            std::cout << "\n\n  " << Color::BoldRed
                      << "[!] Download failed (HTTP " << resp.statusCode << ").\n" << Color::Reset;
            pressAnyKey();
            return;
        }
        eaBytes = std::move(resp.body);
    } catch (std::exception& e) {
        std::cout << "\n\n  " << Color::BoldRed << "[!] Download error: " << e.what() << "\n" << Color::Reset;
        pressAnyKey();
        return;
    }

    std::cout << " " << Color::BoldGreen << "OK" << Color::Reset
              << " (" << eaBytes.size() / 1024 << " KB)\n\n";

    // Step 4: copy to all paths
    std::cout << "  Installing...\n\n";
    int ok = 0, failed = 0;

    for (auto& pathStr : cfg.paths) {
        fs::path dir(pathStr);
        fs::path dest = dir / EA_FILENAME;
        std::cout << "    " << Color::Dim << pathStr << Color::Reset << "\n    ";

        if (!fs::exists(dir)) {
            std::cout << Color::BoldYellow << "[SKIP]" << Color::Reset << " Path does not exist\n\n";
            ++failed;
            continue;
        }

        // Write to temp file then rename (atomic-ish)
        fs::path tmp = dir / (EA_FILENAME + ".tmp");
        try {
            std::ofstream out(tmp, std::ios::binary);
            out.write(eaBytes.data(), (std::streamsize)eaBytes.size());
            out.close();
            if (fs::exists(dest)) fs::remove(dest);
            fs::rename(tmp, dest);
            std::cout << Color::BoldGreen << "[OK]" << Color::Reset << "\n\n";
            ++ok;
        } catch (std::exception& e) {
            if (fs::exists(tmp)) { try { fs::remove(tmp); } catch (...) {} }
            std::cout << Color::BoldRed << "[FAILED]" << Color::Reset
                      << " " << e.what() << "\n\n";
            ++failed;
        }
    }

    printSeparator();
    std::cout << "  " << Color::BoldGreen << ok << " updated" << Color::Reset;
    if (failed > 0)
        std::cout << "  " << Color::BoldRed << failed << " failed" << Color::Reset;
    std::cout << "\n\n";

    if (ok > 0) {
        std::cout << Color::BoldYellow
                  << "  [!] Action required:\n"
                  << "      Remove Augrix EA from every chart and drag it back on.\n"
                  << "      MT5 will not load the new version until the EA is restarted.\n"
                  << Color::Reset << "\n";
    }

    pressAnyKey();
}

// ─── Menu: Add path ──────────────────────────────────────────────────────────

static void menuAddPath(Config& cfg) {
    while (true) {
        clearScreen();
        printHeader();
        printSeparator();
        std::cout << Color::BoldWhite << "  Add Path\n" << Color::Reset;
        printSeparator();
        std::cout << "\n";
        std::cout << "  Enter the full path to your MT5 MQL5\\Experts folder.\n";
        std::cout << "  Example: C:\\Users\\You\\AppData\\Roaming\\MetaQuotes\\Terminal\\<HASH>\\MQL5\\Experts\n\n";
        std::cout << "  " << Color::Dim << "(0 to go back)" << Color::Reset << "\n\n";
        std::cout << "  > ";
        std::cout.flush();

        std::string input;
        std::getline(std::cin, input);

        // Trim whitespace
        while (!input.empty() && isspace((unsigned char)input.front())) input.erase(input.begin());
        while (!input.empty() && isspace((unsigned char)input.back()))  input.pop_back();

        if (input == "0" || input.empty()) return;

        // Check for duplicate
        if (std::find(cfg.paths.begin(), cfg.paths.end(), input) != cfg.paths.end()) {
            std::cout << "\n  " << Color::BoldYellow << "[!] This path is already in the list.\n" << Color::Reset;
            pressAnyKey();
            continue;
        }

        // Validate path exists
        if (!fs::exists(fs::path(input))) {
            std::cout << "\n  " << Color::BoldYellow << "[!] Path does not exist. Add anyway? [y/N] " << Color::Reset;
            std::cout.flush();
            std::string yn;
            std::getline(std::cin, yn);
            if (yn.empty() || tolower(yn[0]) != 'y') continue;
        }

        cfg.paths.push_back(input);
        cfg.savePaths();

        std::cout << "\n  " << Color::BoldGreen << "[OK]" << Color::Reset << " Path added.\n";
        pressAnyKey();
        return;
    }
}

// ─── Menu: Remove path ───────────────────────────────────────────────────────

static void menuRemovePath(Config& cfg) {
    while (true) {
        clearScreen();
        printHeader();
        printSeparator();
        std::cout << Color::BoldWhite << "  Remove Path\n" << Color::Reset;
        printSeparator();
        std::cout << "\n";

        if (cfg.paths.empty()) {
            std::cout << "  " << Color::Yellow << "No paths configured.\n" << Color::Reset;
            pressAnyKey();
            return;
        }

        for (size_t i = 0; i < cfg.paths.size(); ++i) {
            std::cout << "  " << Color::BoldCyan << (i + 1) << Color::Reset
                      << " - " << cfg.paths[i] << "\n";
        }
        std::cout << "\n  " << Color::Dim << "0 - Back" << Color::Reset << "\n\n";
        std::cout << "  Select path to remove: ";
        std::cout.flush();

        std::string input;
        std::getline(std::cin, input);

        int sel = 0;
        try { sel = std::stoi(input); } catch (...) {}

        if (sel == 0) return;
        if (sel < 1 || sel > (int)cfg.paths.size()) {
            std::cout << "\n  " << Color::BoldRed << "[!] Invalid selection.\n" << Color::Reset;
            pressAnyKey();
            continue;
        }

        std::string removed = cfg.paths[sel - 1];
        cfg.paths.erase(cfg.paths.begin() + (sel - 1));
        cfg.savePaths();

        std::cout << "\n  " << Color::BoldGreen << "[OK]" << Color::Reset
                  << " Removed: " << Color::Dim << removed << Color::Reset << "\n";
        pressAnyKey();
        return;
    }
}

// ─── Menu: Set API key ───────────────────────────────────────────────────────

static void menuSetApiKey(Config& cfg) {
    while (true) {
        clearScreen();
        printHeader();
        printSeparator();
        std::cout << Color::BoldWhite << "  Set API Key\n" << Color::Reset;
        printSeparator();
        std::cout << "\n";
        std::cout << "  You can find your API key at:\n";
        std::cout << "  " << Color::Cyan << "https://augrix.io/dashboard" << Color::Reset << "  (Account > API Keys)\n\n";
        if (cfg.hasApiKey()) {
            std::cout << "  Current key: " << Color::BoldGreen << "configured" << Color::Reset << "\n\n";
        }
        std::cout << "  " << Color::Dim << "(0 to go back)" << Color::Reset << "\n\n";
        std::cout << "  Enter API key: ";
        std::cout.flush();

        std::string input;
        std::getline(std::cin, input);

        // Trim
        while (!input.empty() && isspace((unsigned char)input.front())) input.erase(input.begin());
        while (!input.empty() && isspace((unsigned char)input.back()))  input.pop_back();

        if (input == "0" || input.empty()) return;

        if (input.substr(0, 3) != "vx_") {
            std::cout << "\n  " << Color::BoldRed
                      << "[!] Invalid key format. API keys start with 'vx_'.\n" << Color::Reset;
            pressAnyKey();
            continue;
        }

        // Quick connectivity check
        std::cout << "\n  Verifying...  ";
        std::cout.flush();
        try {
            auto resp = Http::get(SERVER_HOST, SERVER_PORT, VERSION_PATH);
            if (resp.statusCode == 200) {
                std::cout << Color::BoldGreen << "Server reachable." << Color::Reset << "\n";
            } else {
                std::cout << Color::Yellow << "Server returned HTTP " << resp.statusCode << "." << Color::Reset << "\n";
            }
        } catch (...) {
            std::cout << Color::Yellow << "Could not reach server (saved anyway)." << Color::Reset << "\n";
        }

        cfg.apiKey = input;
        cfg.saveKey();

        std::cout << "\n  " << Color::BoldGreen << "[OK]" << Color::Reset << " API key saved.\n";
        pressAnyKey();
        return;
    }
}

// ─── Menu: Enable/Disable Auto-Update ────────────────────────────────────────

static void menuAutoUpdate(Config& cfg) {
    clearScreen();
    printHeader();
    printSeparator();
    std::cout << Color::BoldWhite << "  Auto-Update Setup\n" << Color::Reset;
    printSeparator();
    std::cout << "\n";

    if (!cfg.hasApiKey()) {
        std::cout << Color::BoldRed << "  [!] Set your API key first (option 4).\n" << Color::Reset;
        pressAnyKey();
        return;
    }

    std::cout << "  Auto-update installs a Windows Scheduled Task that runs\n";
    std::cout << "  every 5 minutes and checks if your EA requested an update.\n\n";
    std::cout << "  When the EA detects a new version, it waits for a safe moment\n";
    std::cout << "  (no open positions, low volatility), saves its state, then\n";
    std::cout << "  signals the updater. The updater downloads the new version,\n";
    std::cout << "  swaps the file, and restarts MT5 automatically.\n\n";

    std::cout << "  " << Color::BoldCyan << "1" << Color::Reset << " - Enable auto-update (install scheduled task)\n";
    std::cout << "  " << Color::BoldCyan << "2" << Color::Reset << " - Disable auto-update (remove scheduled task)\n";
    std::cout << "  " << Color::Dim << "0 - Back" << Color::Reset << "\n\n";
    std::cout << "  Select option: ";
    std::cout.flush();

    std::string input;
    std::getline(std::cin, input);
    if (input.empty()) return;

    switch (input[0]) {
        case '1': {
            std::cout << "\n  Installing scheduled task...  ";
            std::cout.flush();
            if (installScheduledTask()) {
                std::cout << Color::BoldGreen << "Done!" << Color::Reset << "\n\n";
                std::cout << "  " << Color::Green << "Auto-update is now enabled." << Color::Reset << "\n";
                std::cout << "  The updater will check every 5 minutes for update signals.\n";
            } else {
                std::cout << Color::BoldRed << "Failed!" << Color::Reset << "\n\n";
                std::cout << "  " << Color::Yellow << "Try running the updater as Administrator.\n" << Color::Reset;
            }
            break;
        }
        case '2': {
            std::cout << "\n  Removing scheduled task...  ";
            std::cout.flush();
            if (removeScheduledTask()) {
                std::cout << Color::BoldGreen << "Done!" << Color::Reset << "\n\n";
                std::cout << "  " << Color::Yellow << "Auto-update disabled. You can still update manually (option 1).\n" << Color::Reset;
            } else {
                std::cout << Color::BoldRed << "Failed!" << Color::Reset << "\n\n";
                std::cout << "  " << Color::Yellow << "Task may not exist or you need Administrator privileges.\n" << Color::Reset;
            }
            break;
        }
        default:
            return;
    }

    pressAnyKey();
}

// ─── Main ───────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    enableAnsi();

    // Parse CLI args
    bool watchMode = false;
    bool installTask = false;
    bool removeTask = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        if (arg == "--watch")        { watchMode = true; g_silent = true; }
        else if (arg == "--install-task") installTask = true;
        else if (arg == "--remove-task")  removeTask = true;
    }

    Config cfg = Config::load();

    // --watch: silent flag-file scanner (run by Scheduled Task)
    if (watchMode) {
        return runWatch(cfg);
    }

    // --install-task: register scheduled task and exit
    if (installTask) {
        enableAnsi();
        if (installScheduledTask()) {
            std::cout << "Scheduled task installed successfully.\n";
            return 0;
        } else {
            std::cerr << "Failed to install scheduled task. Try running as Administrator.\n";
            return 1;
        }
    }

    // --remove-task: remove scheduled task and exit
    if (removeTask) {
        enableAnsi();
        if (removeScheduledTask()) {
            std::cout << "Scheduled task removed.\n";
            return 0;
        } else {
            std::cerr << "Failed to remove scheduled task.\n";
            return 1;
        }
    }

    // Interactive mode
    SetConsoleTitleW(L"Augrix EA Autoupdater v2.0");
    std::string errorMsg;

    while (true) {
        clearScreen();
        printMainHeader(cfg);

        if (!errorMsg.empty()) {
            std::cout << "  " << Color::BoldRed << "[!] " << errorMsg << Color::Reset << "\n\n";
            errorMsg.clear();
        }

        std::cout << "  " << Color::BoldCyan << "1" << Color::Reset << " - Update the bots\n";
        std::cout << "  " << Color::BoldCyan << "2" << Color::Reset << " - Add path\n";
        std::cout << "  " << Color::BoldCyan << "3" << Color::Reset << " - Remove path\n";
        std::cout << "  " << Color::BoldCyan << "4" << Color::Reset << " - Set API key\n";
        std::cout << "  " << Color::BoldCyan << "5" << Color::Reset << " - Auto-update setup\n";
        std::cout << "  " << Color::Dim << "0 - Exit" << Color::Reset << "\n\n";
        printSeparator();
        std::cout << "\n  Select option: ";
        std::cout.flush();

        std::string input;
        std::getline(std::cin, input);

        if (input.empty()) continue;

        char ch = input[0];
        switch (ch) {
            case '1': menuUpdate(cfg); break;
            case '2': menuAddPath(cfg); break;
            case '3': menuRemovePath(cfg); break;
            case '4': menuSetApiKey(cfg); break;
            case '5': menuAutoUpdate(cfg); break;
            case '0':
                clearScreen();
                std::cout << "\n  " << Color::Dim << "Goodbye.\n" << Color::Reset << "\n";
                return 0;
            default:
                errorMsg = "Invalid option '" + input + "'";
                break;
        }

        // Reload config (may have changed in submenus)
        cfg = Config::load();
    }
}
