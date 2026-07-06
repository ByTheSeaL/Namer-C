// UTF-8 <-> UTF-16 helpers and config-file paths (%APPDATA%\Namer).
//
// Uses the same directory and file names as the Python Namer, so settings
// and the API key carry over between the two on Windows.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>

#include <string>

inline std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

inline std::string narrow(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

inline std::wstring configDir() {
    wchar_t path[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, path)))
        return L".";
    std::wstring dir = std::wstring(path) + L"\\Namer";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

// Win32 file I/O (not <fstream>): MinGW's libstdc++ lacks the wide-path
// fstream constructor that MSVC ships, so we use CreateFileW directly to
// keep both toolchains happy and to handle non-ASCII paths correctly.
inline std::string readFileUtf8(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return "";
    std::string out;
    char buf[8192];
    DWORD got = 0;
    while (ReadFile(f, buf, sizeof buf, &got, nullptr) && got > 0)
        out.append(buf, got);
    CloseHandle(f);
    return out;
}

inline bool writeFileUtf8(const std::wstring& path, const std::string& content) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = !content.empty()
                  ? WriteFile(f, content.data(), (DWORD)content.size(), &written,
                              nullptr) && written == content.size()
                  : true;
    CloseHandle(f);
    return ok;
}

// Text of an edit/combo/static control as UTF-8.
inline std::string getWindowTextUtf8(HWND hwnd) {
    int len = GetWindowTextLengthW(hwnd);
    std::wstring w(len, 0);
    GetWindowTextW(hwnd, &w[0], len + 1);
    return narrow(w);
}
