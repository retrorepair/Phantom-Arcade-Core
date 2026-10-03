/**
 * ============================================================================
 * Phantom Arcade - Windows Setup & Management Application
 * File: PhantomArcadeManager.cpp
 * Language: C++17 / Native Win32
 * 
 * Description:
 *   Native Windows desktop setup tool and daemon launcher for the Phantom Arcade
 *   Groovy_MiSTer bridge. Supports GroovyMAME (Calamity 15kHz native streaming),
 *   RetroArch (SwitchRes multi-core CRT), Dolphin, Flycast, and PCSX2.
 *   Dynamic UDP port configuration (default 1999), LAN auto-discovery, persistent
 *   registry/JSON configuration, and live process launching on MiSTer UDP trigger.
 * ============================================================================
 */

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <atomic>
#include <filesystem>
#include <algorithm>
#include <map>
#include <mutex>
#include <set>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")

namespace fs = std::filesystem;

// Control IDs
#define IDC_EDIT_MISTER_IP          101
#define IDC_EDIT_UDP_PORT           102
#define IDC_EDIT_GROOVYMAME_EXE     103
#define IDC_BTN_BROWSE_MAME_EXE     104
#define IDC_EDIT_MAME_ROMS          105
#define IDC_BTN_BROWSE_MAME_ROMS    106
#define IDC_EDIT_RETROARCH_EXE      107
#define IDC_BTN_BROWSE_RA_EXE       108
#define IDC_EDIT_RETROARCH_ROMS     109
#define IDC_BTN_BROWSE_RA_ROMS      110
#define IDC_EDIT_DOLPHIN_EXE        111
#define IDC_BTN_BROWSE_DOLPHIN      112
#define IDC_EDIT_GC_ROMS            113
#define IDC_BTN_BROWSE_GCROMS       114
#define IDC_EDIT_FLYCAST_EXE        115
#define IDC_BTN_BROWSE_FLYCAST      116
#define IDC_EDIT_NAOMI_ROMS         117
#define IDC_BTN_BROWSE_NAOMI        118
#define IDC_EDIT_PCSX2_EXE          119
#define IDC_BTN_BROWSE_PCSX2        120
#define IDC_EDIT_PS2_ROMS           121
#define IDC_BTN_BROWSE_PS2ROMS      122
#define IDC_BTN_SCAN_ROMS           123
#define IDC_BTN_TEST_MISTER         124
#define IDC_BTN_SAVE_CONFIG         125
#define IDC_BTN_TOGGLE_DAEMON       126
#define IDC_STATIC_STATUS           127
#define IDC_LIST_GAMES              128
#define IDC_BTN_LAUNCH_GAME         129
#define IDC_EDIT_LAUNCH_DELAY       130

// Global State
HINSTANCE hInst = NULL;
HWND hMainWnd = NULL;
HWND hEditLaunchDelay;
HWND hStaticMisterIp = NULL;   // read-only: the address we discovered
HWND hEditMameExe, hEditMameRoms;
HWND hEditRetroarchExe, hEditRetroarchRoms;
HWND hEditDolphinExe, hEditGcRoms;
HWND hEditFlycastExe, hEditNaomiRoms;
HWND hEditPcsx2Exe, hEditPs2Roms;
HWND hStaticStatus, hListGames;
HWND hBtnToggleDaemon, hBtnLaunchGame;

std::atomic<bool> g_daemonRunning(false);
std::atomic<DWORD> g_activePid(0);
std::atomic<int> g_configuredPort(1999);
SOCKET g_udpSocket = INVALID_SOCKET;
SOCKET g_httpSocket = INVALID_SOCKET;
HANDLE g_hDaemonThread = NULL;
HANDLE g_hHttpThread = NULL;

// Forward declarations
void LoadConfiguration();
void SaveConfiguration();
void ScanRomDirectories();
void ToggleDaemon();
bool LaunchGame(const std::string& gameId, const std::wstring& targetMisterIp = L"");

// Helper: Application Directory (Guarantees config is never saved to random browse folders)
std::wstring GetAppDir() {
    wchar_t exePath[MAX_PATH] = { 0 };
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    std::wstring path(exePath);
    size_t pos = path.find_last_of(L"\\/");
    if (pos != std::wstring::npos) {
        return path.substr(0, pos);
    }
    return L".";
}

std::wstring GetConfigPath() {
    return GetAppDir() + L"\\phantom_config.json";
}

// Registry Persistence Helpers (HKCU\\Software\\PhantomArcade)
void RegWriteString(const wchar_t* valueName, const std::wstring& value) {
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\PhantomArcade", 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(hKey, valueName, 0, REG_SZ, (const BYTE*)value.c_str(), (DWORD)((value.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(hKey);
    }
}

std::wstring RegReadString(const wchar_t* valueName, const std::wstring& defaultVal = L"") {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\PhantomArcade", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        wchar_t buffer[1024] = { 0 };
        DWORD dwSize = sizeof(buffer);
        DWORD dwType = REG_SZ;
        if (RegQueryValueExW(hKey, valueName, NULL, &dwType, (LPBYTE)buffer, &dwSize) == ERROR_SUCCESS) {
            RegCloseKey(hKey);
            return std::wstring(buffer);
        }
        RegCloseKey(hKey);
    }
    return defaultVal;
}

void RegWriteInt(const wchar_t* valueName, int value) {
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\PhantomArcade", 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        DWORD dw = (DWORD)value;
        RegSetValueExW(hKey, valueName, 0, REG_DWORD, (const BYTE*)&dw, sizeof(DWORD));
        RegCloseKey(hKey);
    }
}

int RegReadInt(const wchar_t* valueName, int defaultVal) {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\PhantomArcade", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD dw = 0;
        DWORD dwSize = sizeof(dw);
        DWORD dwType = REG_DWORD;
        if (RegQueryValueExW(hKey, valueName, NULL, &dwType, (LPBYTE)&dw, &dwSize) == ERROR_SUCCESS) {
            RegCloseKey(hKey);
            return (int)dw;
        }
        RegCloseKey(hKey);
    }
    return defaultVal;
}

// Helper: Browse for Folder
std::wstring BrowseFolder(HWND hWnd, const wchar_t* title) {
    std::wstring result = L"";
    BROWSEINFO bi = { 0 };
    bi.lpszTitle = title;
    bi.hwndOwner = hWnd;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolder(&bi);
    if (pidl != 0) {
        wchar_t path[MAX_PATH];
        if (SHGetPathFromIDList(pidl, path)) {
            result = path;
        }
        CoTaskMemFree(pidl);
    }
    // Always restore working directory to application dir
    SetCurrentDirectoryW(GetAppDir().c_str());
    return result;
}

// Helper: Browse for Executable (with OFN_NOCHANGEDIR so current directory never breaks)
std::wstring BrowseFile(HWND hWnd, const wchar_t* filter) {
    wchar_t filename[MAX_PATH] = { 0 };
    OPENFILENAME ofn = { 0 };
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hWnd;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileName(&ofn)) {
        SetCurrentDirectoryW(GetAppDir().c_str());
        return filename;
    }
    SetCurrentDirectoryW(GetAppDir().c_str());
    return L"";
}

// Helper: Read Window Text
std::wstring GetText(HWND hWnd) {
    int len = GetWindowTextLength(hWnd);
    if (len <= 0) return L"";
    std::vector<wchar_t> buf(len + 1);
    GetWindowText(hWnd, buf.data(), len + 1);
    return std::wstring(buf.data());
}

// Helper: UTF-8 and JSON String escaping
std::string ToJsonString(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string s(sizeNeeded, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), &s[0], sizeNeeded, NULL, NULL);
    std::string out = "";
    for (char c : s) {
        if (c == '\\') out += "/";
        else if (c == '"') out += "\\\"";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else out += c;
    }
    return out;
}

std::wstring StringToWstring(const std::string& s) {
    if (s.empty()) return L"";
    int sizeNeeded = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
    if (sizeNeeded <= 0) return std::wstring(s.begin(), s.end());
    std::wstring wstr(sizeNeeded, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &wstr[0], sizeNeeded);
    return wstr;
}

// Robust JSON key-value extractors
std::string ExtractJsonString(const std::string& json, const std::string& key) {
    std::string searchKey = "\"" + key + "\"";
    size_t pos = 0;
    while ((pos = json.find(searchKey, pos)) != std::string::npos) {
        size_t colon = json.find(":", pos + searchKey.length());
        if (colon == std::string::npos) break;
        size_t quoteStart = json.find("\"", colon + 1);
        if (quoteStart == std::string::npos) break;
        
        bool ok = true;
        for (size_t i = colon + 1; i < quoteStart; ++i) {
            char ch = json[i];
            if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') {
                ok = false;
                break;
            }
        }
        if (!ok) {
            pos += searchKey.length();
            continue;
        }

        std::string val = "";
        size_t p = quoteStart + 1;
        while (p < json.length()) {
            if (json[p] == '\\' && p + 1 < json.length()) {
                char nextC = json[p + 1];
                if (nextC == '\\') { val += "\\"; p += 2; }
                else if (nextC == '"') { val += "\""; p += 2; }
                else if (nextC == '/') { val += "/"; p += 2; }
                else if (nextC == 'n') { val += "\n"; p += 2; }
                else if (nextC == 'r') { val += "\r"; p += 2; }
                else if (nextC == 't') { val += "\t"; p += 2; }
                else { val += nextC; p += 2; }
            } else if (json[p] == '"') {
                break;
            } else {
                val += json[p++];
            }
        }
        return val;
    }
    return "";
}

int ExtractJsonInt(const std::string& json, const std::string& key, int defaultVal) {
    std::string searchKey = "\"" + key + "\"";
    size_t pos = json.find(searchKey);
    if (pos == std::string::npos) return defaultVal;
    pos = json.find(":", pos + searchKey.length());
    if (pos == std::string::npos) return defaultVal;
    
    size_t numStart = json.find_first_of("-0123456789", pos + 1);
    if (numStart == std::string::npos) return defaultVal;
    size_t numEnd = json.find_first_not_of("0123456789", numStart + 1);
    
    try {
        std::string numStr = json.substr(numStart, numEnd - numStart);
        return std::stoi(numStr);
    } catch (...) {
        return defaultVal;
    }
}

// ---- MiSTer address and port ------------------------------------------------------
//
// Neither of these is a setting any more, but for different reasons, and the difference
// matters:
//
//   The address is genuinely discovered. The MiSTer opens every conversation - it
//   broadcasts DISCOVER_PHANTOM and sends LAUNCH - so the daemon simply remembers the
//   source address of whatever arrived and hands that to GroovyMAME as -mister_ip. It
//   follows the cabinet across DHCP leases on its own, and a typed value could only
//   ever be wrong. There is nothing to configure.
//
//   The port cannot be discovered, because discovery itself has to arrive somewhere.
//   It is a rendezvous both ends agree on in advance, so it is a constant here and in
//   the core (phantom.ini UDP_PORT). It is not exposed as a control because changing it
//   on one side alone silently breaks discovery, which is a far more likely outcome
//   than a clash on 1999. phantom_config.json's "udp_port" is still honoured for the
//   rare case where something else owns the port and both ends get changed together.
#define PHANTOM_DEFAULT_PORT 1999

static int g_udpPort = PHANTOM_DEFAULT_PORT;

static std::mutex g_ipMutex;
static std::wstring g_learnedMisterIp;   // last address a MiSTer contacted us from

int GetPort() { return g_udpPort; }

static std::wstring GetLearnedMisterIp() {
    std::lock_guard<std::mutex> lk(g_ipMutex);
    return g_learnedMisterIp;
}

// Called from the UDP thread for every datagram a MiSTer sends us.
static void NoteMisterAddress(const std::wstring& ip) {
    {
        std::lock_guard<std::mutex> lk(g_ipMutex);
        if (g_learnedMisterIp == ip) return;
        g_learnedMisterIp = ip;
    }
    RegWriteString(L"mister_client_ip", ip);
    if (hStaticMisterIp) {
        std::wstring shown = ip + L"  (discovered)";
        SetWindowText(hStaticMisterIp, shown.c_str());
    }
}

// Load Configuration (Checks JSON file in AppDir, then merges with Registry)
void LoadConfiguration() {
    std::wstring cfgPath = GetConfigPath();
    std::string json = "";
    std::ifstream in; in.open(cfgPath.c_str());
    if (in.is_open()) {
        std::stringstream ss;
        ss << in.rdbuf();
        json = ss.str();
        in.close();
    }

    // 1. Last discovered MiSTer address. Only a seed for the manual Launch button
    //    before any cabinet has been heard from; a real MiSTer overwrites it the
    //    moment it broadcasts. Not shown as an editable field - see GetPort above.
    std::string misterIp = ExtractJsonString(json, "mister_client_ip");
    std::wstring wMisterIp = misterIp.empty() ? RegReadString(L"mister_client_ip", L"") : StringToWstring(misterIp);
    if (!wMisterIp.empty()) {
        {
            std::lock_guard<std::mutex> lk(g_ipMutex);
            g_learnedMisterIp = wMisterIp;
        }
        if (hStaticMisterIp) {
            std::wstring shown = wMisterIp + L"  (last seen)";
            SetWindowText(hStaticMisterIp, shown.c_str());
        }
    }

    // 2. Rendezvous port. Honoured if present so a clash can be worked around, but both
    //    ends have to be changed together, so it is deliberately not a control.
    g_udpPort = ExtractJsonInt(json, "udp_port", RegReadInt(L"udp_port", PHANTOM_DEFAULT_PORT));
    if (g_udpPort <= 0 || g_udpPort > 65535) g_udpPort = PHANTOM_DEFAULT_PORT;

    // 2b. PC-side launch delay. Zero by default - the merged Phantom Arcade core does not
    // reconfigure the FPGA at launch time, so there is nothing to wait for.
    int delaySec = ExtractJsonInt(json, "launch_delay_sec", RegReadInt(L"launch_delay_sec", 0));
    if (delaySec < 0) delaySec = 0;
    SetWindowText(hEditLaunchDelay, std::to_wstring(delaySec).c_str());

    // 3. GroovyMAME Executable & ROMs
    std::string mameExe = ExtractJsonString(json, "mame_exe");
    if (mameExe.empty()) mameExe = ExtractJsonString(json, "exe");
    std::wstring wMameExe = mameExe.empty() ? RegReadString(L"mame_exe", L"C:\\Emulators\\GroovyMAME\\groovymame64.exe") : StringToWstring(mameExe);
    SetWindowText(hEditMameExe, wMameExe.c_str());

    std::string mameRoms = ExtractJsonString(json, "mame_roms");
    if (mameRoms.empty()) mameRoms = ExtractJsonString(json, "roms");
    std::wstring wMameRoms = mameRoms.empty() ? RegReadString(L"mame_roms", L"C:\\Emulators\\GroovyMAME\\roms") : StringToWstring(mameRoms);
    SetWindowText(hEditMameRoms, wMameRoms.c_str());

    // 4. RetroArch
    std::string raExe = ExtractJsonString(json, "retroarch_exe");
    std::wstring wRaExe = raExe.empty() ? RegReadString(L"retroarch_exe", L"C:\\Emulators\\RetroArch\\retroarch.exe") : StringToWstring(raExe);
    SetWindowText(hEditRetroarchExe, wRaExe.c_str());

    std::string raRoms = ExtractJsonString(json, "retroarch_roms");
    std::wstring wRaRoms = raRoms.empty() ? RegReadString(L"retroarch_roms", L"C:\\Games\\RetroArch\\roms") : StringToWstring(raRoms);
    SetWindowText(hEditRetroarchRoms, wRaRoms.c_str());

    // 5. Dolphin
    std::string dolphinExe = ExtractJsonString(json, "dolphin_exe");
    std::wstring wDolphinExe = dolphinExe.empty() ? RegReadString(L"dolphin_exe", L"C:\\Emulators\\Dolphin\\Dolphin.exe") : StringToWstring(dolphinExe);
    SetWindowText(hEditDolphinExe, wDolphinExe.c_str());

    std::string dolphinRoms = ExtractJsonString(json, "dolphin_roms");
    std::wstring wDolphinRoms = dolphinRoms.empty() ? RegReadString(L"dolphin_roms", L"C:\\Games\\GameCube") : StringToWstring(dolphinRoms);
    SetWindowText(hEditGcRoms, wDolphinRoms.c_str());

    // 6. Flycast
    std::string flycastExe = ExtractJsonString(json, "flycast_exe");
    std::wstring wFlycastExe = flycastExe.empty() ? RegReadString(L"flycast_exe", L"C:\\Emulators\\Flycast\\flycast.exe") : StringToWstring(flycastExe);
    SetWindowText(hEditFlycastExe, wFlycastExe.c_str());

    std::string flycastRoms = ExtractJsonString(json, "flycast_roms");
    std::wstring wFlycastRoms = flycastRoms.empty() ? RegReadString(L"flycast_roms", L"C:\\Games\\Arcade\\Naomi") : StringToWstring(flycastRoms);
    SetWindowText(hEditNaomiRoms, wFlycastRoms.c_str());

    // 7. PCSX2
    std::string pcsx2Exe = ExtractJsonString(json, "pcsx2_exe");
    std::wstring wPcsx2Exe = pcsx2Exe.empty() ? RegReadString(L"pcsx2_exe", L"C:\\Emulators\\PCSX2\\pcsx2-qt.exe") : StringToWstring(pcsx2Exe);
    SetWindowText(hEditPcsx2Exe, wPcsx2Exe.c_str());

    std::string pcsx2Roms = ExtractJsonString(json, "pcsx2_roms");
    std::wstring wPcsx2Roms = pcsx2Roms.empty() ? RegReadString(L"pcsx2_roms", L"C:\\Games\\PS2") : StringToWstring(pcsx2Roms);
    SetWindowText(hEditPs2Roms, wPcsx2Roms.c_str());

    SetWindowText(hStaticStatus, L"Status: Configuration loaded from disk and registry.");
}

// Save Configuration to phantom_config.json AND Windows Registry
void SaveConfiguration() {
    std::wstring misterIp = GetLearnedMisterIp();
    int port = GetPort();
    std::wstring mameExe = GetText(hEditMameExe);
    std::wstring mameRoms = GetText(hEditMameRoms);
    std::wstring raExe = GetText(hEditRetroarchExe);
    std::wstring raRoms = GetText(hEditRetroarchRoms);
    std::wstring dolphinExe = GetText(hEditDolphinExe);
    std::wstring dolphinRoms = GetText(hEditGcRoms);
    std::wstring flycastExe = GetText(hEditFlycastExe);
    std::wstring flycastRoms = GetText(hEditNaomiRoms);
    std::wstring pcsx2Exe = GetText(hEditPcsx2Exe);
    std::wstring pcsx2Roms = GetText(hEditPs2Roms);

    // Save to Windows Registry
    RegWriteString(L"mister_client_ip", misterIp);
    RegWriteInt(L"udp_port", port);

    int launchDelay = 0;
    if (hEditLaunchDelay != NULL) {
        std::wstring dStr = GetText(hEditLaunchDelay);
        if (!dStr.empty()) {
            try { launchDelay = std::stoi(dStr); } catch (...) {}
        }
    }
    if (launchDelay < 0) launchDelay = 0;
    RegWriteInt(L"launch_delay_sec", launchDelay);
    RegWriteString(L"mame_exe", mameExe);
    RegWriteString(L"mame_roms", mameRoms);
    RegWriteString(L"retroarch_exe", raExe);
    RegWriteString(L"retroarch_roms", raRoms);
    RegWriteString(L"dolphin_exe", dolphinExe);
    RegWriteString(L"dolphin_roms", dolphinRoms);
    RegWriteString(L"flycast_exe", flycastExe);
    RegWriteString(L"flycast_roms", flycastRoms);
    RegWriteString(L"pcsx2_exe", pcsx2Exe);
    RegWriteString(L"pcsx2_roms", pcsx2Roms);

    // Save to JSON in application directory
    std::wstring cfgPath = GetConfigPath();
    std::ofstream out; out.open(cfgPath.c_str());
    if (out.is_open()) {
        out << "{\n";
        out << "  \"server\": {\n";
        out << "    \"listen_ip\": \"0.0.0.0\",\n";
        out << "    \"udp_port\": " << port << ",\n";
        out << "    \"launch_delay_sec\": " << launchDelay << ",\n";
        out << "    \"http_port\": 8088,\n";
        out << "    \"mister_client_ip\": \"" << ToJsonString(misterIp) << "\"\n";
        out << "  },\n";
        out << "  \"paths\": {\n";
        out << "    \"mame_exe\": \"" << ToJsonString(mameExe) << "\",\n";
        out << "    \"mame_roms\": \"" << ToJsonString(mameRoms) << "\",\n";
        out << "    \"retroarch_exe\": \"" << ToJsonString(raExe) << "\",\n";
        out << "    \"retroarch_roms\": \"" << ToJsonString(raRoms) << "\",\n";
        out << "    \"dolphin_exe\": \"" << ToJsonString(dolphinExe) << "\",\n";
        out << "    \"dolphin_roms\": \"" << ToJsonString(dolphinRoms) << "\",\n";
        out << "    \"flycast_exe\": \"" << ToJsonString(flycastExe) << "\",\n";
        out << "    \"flycast_roms\": \"" << ToJsonString(flycastRoms) << "\",\n";
        out << "    \"pcsx2_exe\": \"" << ToJsonString(pcsx2Exe) << "\",\n";
        out << "    \"pcsx2_roms\": \"" << ToJsonString(pcsx2Roms) << "\"\n";
        out << "  },\n";
        out << "  \"emulators\": {\n";
        out << "    \"groovymame\": {\n";
        out << "      \"exe\": \"" << ToJsonString(mameExe) << "\",\n";
        out << "      \"roms\": \"" << ToJsonString(mameRoms) << "\",\n";
        out << "      \"args\": \"\\\"{rom_stem}\\\" -video mister -skip_gameinfo -nokeepaspect\",\n";
        out << "      \"pipeline\": \"Groovy_MiSTer SwitchRes 15kHz Direct\"\n";
        out << "    },\n";
        out << "    \"retroarch\": {\n";
        out << "      \"exe\": \"" << ToJsonString(raExe) << "\",\n";
        out << "      \"roms\": \"" << ToJsonString(raRoms) << "\",\n";
        out << "      \"args\": \"-f \\\"{rom}\\\"\",\n";
        out << "      \"pipeline\": \"RetroArch CRT SwitchRes 15kHz\"\n";
        out << "    },\n";
        out << "    \"dolphin\": {\n";
        out << "      \"exe\": \"" << ToJsonString(dolphinExe) << "\",\n";
        out << "      \"roms\": \"" << ToJsonString(dolphinRoms) << "\",\n";
        out << "      \"args\": \"-b -e \\\"{rom}\\\"\",\n";
        out << "      \"pipeline\": \"Groovy_MiSTer 480i/240p\"\n";
        out << "    },\n";
        out << "    \"flycast\": {\n";
        out << "      \"exe\": \"" << ToJsonString(flycastExe) << "\",\n";
        out << "      \"roms\": \"" << ToJsonString(flycastRoms) << "\",\n";
        out << "      \"args\": \"\\\"{rom}\\\"\",\n";
        out << "      \"pipeline\": \"SwitchRes Direct 15kHz\"\n";
        out << "    },\n";
        out << "    \"pcsx2\": {\n";
        out << "      \"exe\": \"" << ToJsonString(pcsx2Exe) << "\",\n";
        out << "      \"roms\": \"" << ToJsonString(pcsx2Roms) << "\",\n";
        out << "      \"args\": \"-batch -nogui \\\"{rom}\\\"\",\n";
        out << "      \"pipeline\": \"PCSX2 CRT 240p/480i\"\n";
        out << "    }\n";
        out << "  }\n";
        out << "}\n";
        out.close();

        std::wstring msg = L"Status: Configuration saved permanently to disk and registry (Port: " + std::to_wstring(port) + L").";
        SetWindowText(hStaticStatus, msg.c_str());
    } else {
        SetWindowText(hStaticStatus, L"Error: Failed to write phantom_config.json.");
    }
}

// ---- MAME title lookup -----------------------------------------------------------
//
// A scan used to title every arcade game by its ROM filename, so the cabinet listed
// "bgaregga" and "ddp3" rather than what they are. MAME already knows the real names,
// so ask it: `mame -listfull` prints "shortname  \"Full Description\"" for everything
// it supports, and the description carries the region - exactly what is wanted on the
// menu. Nothing is shipped or redistributed; this reads the user's own MAME install.
//
// The output is a few megabytes and takes a moment, so it is cached beside the exe and
// only regenerated when the cache is missing.

static std::string RunCapture(const std::wstring& cmdline, const std::wstring& workDir) {
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE rd = NULL, wr = NULL;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return "";
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError = wr;
    PROCESS_INFORMATION pi = { 0 };

    std::vector<wchar_t> buf(cmdline.begin(), cmdline.end());
    buf.push_back(0);

    BOOL ok = CreateProcessW(NULL, buf.data(), NULL, NULL, TRUE, CREATE_NO_WINDOW,
                             NULL, workDir.empty() ? NULL : workDir.c_str(), &si, &pi);
    CloseHandle(wr);   // the child owns the write end now; our copy must go or the read never ends
    if (!ok) { CloseHandle(rd); return ""; }

    std::string out;
    char chunk[8192];
    DWORD got = 0;
    while (ReadFile(rd, chunk, sizeof(chunk), &got, NULL) && got > 0) out.append(chunk, got);

    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, 20000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return out;
}

// MAME appends a compiler-style build stamp to some descriptions:
//   "Battle Garegga (Europe / USA / Japan / Asia) (Sat Feb 3 1996)"
// The region is wanted on the menu, the build stamp is not.
//
// Testing for a year alone is far too greedy, because plenty of real region groups
// carry one and must survive intact:
//   "DoDonPachi III (World, 2002.05.15 Master Ver)"
//   "Deathsmiles (Japan, 2007/10/09 MASTER VER)"
// A MAME build stamp is specifically "(Weekday Mon D YYYY)": it always contains an
// English month abbreviation and never a comma, which neither of those does. Requiring
// all three - year, month name, no comma - strips the stamp and leaves regions alone.
static std::string TrimBuildDate(const std::string& desc) {
    if (desc.size() < 3 || desc.back() != ')') return desc;
    size_t open = desc.rfind('(');
    if (open == std::string::npos || open == 0) return desc;

    std::string tail = desc.substr(open + 1, desc.size() - open - 2);
    if (tail.find(',') != std::string::npos) return desc;

    bool hasYear = false;
    for (size_t i = 0; i + 4 <= tail.size(); i++) {
        if ((tail[i] == '1' && tail[i + 1] == '9') || (tail[i] == '2' && tail[i + 1] == '0')) {
            if (isdigit((unsigned char)tail[i + 2]) && isdigit((unsigned char)tail[i + 3])) { hasYear = true; break; }
        }
    }
    if (!hasYear) return desc;

    // The group must be nothing BUT a date. Every run of letters in it has to be a month,
    // a weekday or an ordinal suffix; anything else means the brackets are carrying
    // information worth keeping, and dropping them would merge distinct sets:
    //   "Judge Dredd (Rev B Nov. 26 1997)" and "(Rev C Dec. 17 1997)" -> two "Judge Dredd"
    //   "Vapor TRX (GUTS Apr 10 1998 / MAIN Apr 10 1998)"
    //   "Pump it Up: The Collection (R5/v3.43 - Nov 14 2000)"
    // Checking whole words also keeps "Apple IIgs (1991 Mark Twain prototype)", where a
    // substring search finds "Mar" inside "Mark".
    static const char* dateWords[] = {
        "jan","feb","mar","apr","may","jun","jul","aug","sep","oct","nov","dec",
        "mon","tue","wed","thu","fri","sat","sun",
        "st","nd","rd","th"
    };

    bool hasMonth = false;
    for (size_t i = 0; i < tail.size(); ) {
        if (!isalpha((unsigned char)tail[i])) { i++; continue; }
        size_t j = i;
        while (j < tail.size() && isalpha((unsigned char)tail[j])) j++;

        std::string word = tail.substr(i, j - i);
        for (char& c : word) c = (char)tolower((unsigned char)c);

        bool known = false, isMonth = false;
        for (const char* w : dateWords) {
            if (word == w) {
                known = true;
                isMonth = (word.size() == 3 && word != "mon" && word != "tue" && word != "wed" &&
                           word != "thu" && word != "fri" && word != "sat" && word != "sun");
                break;
            }
        }
        if (!known) return desc;        // a real word: keep the whole group
        if (isMonth) hasMonth = true;
        i = j;
    }
    if (!hasMonth) return desc;

    size_t end = open;
    while (end > 0 && desc[end - 1] == ' ') end--;
    return desc.substr(0, end);
}

static std::map<std::string, std::string> LoadMameTitles(const std::wstring& mameExe) {
    std::map<std::string, std::string> titles;
    if (mameExe.empty()) return titles;

    std::wstring cachePath = GetAppDir() + L"\\mame_titles.txt";
    std::string raw;

    std::ifstream cache(cachePath.c_str(), std::ios::binary);
    if (cache.is_open()) {
        std::stringstream ss; ss << cache.rdbuf(); raw = ss.str(); cache.close();
    }

    if (raw.size() < 1024) {
        std::wstring dir;
        size_t slash = mameExe.find_last_of(L"\\/");
        if (slash != std::wstring::npos) dir = mameExe.substr(0, slash);
        raw = RunCapture(L"\"" + mameExe + L"\" -listfull", dir);
        if (raw.size() >= 1024) {
            std::ofstream out(cachePath.c_str(), std::ios::binary);
            if (out.is_open()) { out << raw; out.close(); }
        }
    }

    // lines look like:   bgaregga         "Battle Garegga (Europe / ...) (Sat Feb 3 1996)"
    std::stringstream ls(raw);
    std::string line;
    while (std::getline(ls, line)) {
        size_t q1 = line.find('"');
        if (q1 == std::string::npos) continue;
        size_t q2 = line.rfind('"');
        if (q2 == std::string::npos || q2 <= q1 + 1) continue;

        std::string name = line.substr(0, q1);
        while (!name.empty() && (name.back() == ' ' || name.back() == '\t' || name.back() == '\r')) name.pop_back();
        if (name.empty() || name == "Name:") continue;

        titles[name] = TrimBuildDate(line.substr(q1 + 1, q2 - q1 - 1));
    }
    return titles;
}

// ---- which arcade sets will actually start -----------------------------------------
//
// A roms folder is not a games list. Two kinds of entry in one will start MAME and have
// it exit again within a second, which from the cabinet is indistinguishable from the
// stream failing - the launcher just sits on "starting stream" and gives up:
//
//   Incomplete sets. "mame -verifyroms" calls these bad; a missing parent, BIOS or
//   device ROM is enough. ("best available" is fine - it runs, something is just
//   undumped.)
//
//   Things that are not games. Devices and BIOS images live in roms folders quite
//   legitimately - hd44780 is an LCD controller, model1io is Sega I/O board firmware.
//   MAME flags them in -listxml as runnable="no".
//
// Both answers come from the user's own MAME, asked once per scan, and a set is listed
// only if it passes both. Anything we cannot get an answer about is kept rather than
// hidden, so a parsing failure cannot silently empty somebody's library.
static void FilterRunnableSets(const std::wstring& mameExe,
                               const std::vector<std::string>& stems,
                               std::set<std::string>& outRunnable) {
    outRunnable.insert(stems.begin(), stems.end());   // default to keeping everything
    if (mameExe.empty() || stems.empty()) return;

    std::wstring dir;
    size_t slash = mameExe.find_last_of(L"\\/");
    if (slash != std::wstring::npos) dir = mameExe.substr(0, slash);

    std::wstring names;
    for (const auto& s : stems) names += L" " + StringToWstring(s);

    // 1. bad romsets
    std::string verify = RunCapture(L"\"" + mameExe + L"\" -verifyroms" + names, dir);
    std::stringstream vs(verify);
    std::string line;
    while (std::getline(vs, line)) {
        // "romset <name> is bad"
        if (line.rfind("romset ", 0) != 0) continue;
        size_t sp = line.find(' ', 7);
        if (sp == std::string::npos) continue;
        std::string name = line.substr(7, sp - 7);
        if (line.find(" is bad") != std::string::npos) outRunnable.erase(name);
    }

    // 2. devices and other non-runnable machines
    std::string xml = RunCapture(L"\"" + mameExe + L"\" -listxml" + names, dir);
    if (xml.size() > 64) {
        size_t at = 0;
        while ((at = xml.find("<machine name=\"", at)) != std::string::npos) {
            size_t q = at + 15;
            size_t e = xml.find('"', q);
            if (e == std::string::npos) break;
            std::string name = xml.substr(q, e - q);
            size_t tagEnd = xml.find('>', e);
            if (tagEnd == std::string::npos) break;
            std::string tag = xml.substr(e, tagEnd - e);
            if (tag.find("runnable=\"no\"") != std::string::npos) outRunnable.erase(name);
            at = tagEnd;
        }
    }
}

static std::string JsonEscape(const std::string& in) {
    std::string out;
    for (char c : in) {
        if (c == '"' || c == '\\') { out.push_back('\\'); out.push_back(c); }
        else if ((unsigned char)c < 0x20) out.push_back(' ');
        else out.push_back(c);
    }
    return out;
}

// Auto-Scan ROM Directories and build games_catalog.json
void ScanRomDirectories() {
    SendMessage(hListGames, LB_RESETCONTENT, 0, 0);

    struct ScanTarget {
        std::wstring path;
        std::string system;
        std::string systemName;
        std::string videoMode;
        std::string resolution;
    };

    std::vector<ScanTarget> targets = {
        { GetText(hEditMameRoms), "groovymame", "GroovyMAME Arcade", "15kHz 240p Native", "Dynamic SwitchRes" },
        { GetText(hEditRetroarchRoms), "retroarch", "RetroArch SwitchRes", "15kHz 240p Dynamic", "Dynamic SwitchRes" },
        { GetText(hEditGcRoms), "dolphin", "GameCube / Wii", "15kHz 480i @ 60Hz", "640x480i" },
        { GetText(hEditNaomiRoms), "flycast", "Sega Naomi / DC Arcade", "15kHz 240p / 480i", "640x480" },
        { GetText(hEditPs2Roms), "pcsx2", "Sony PlayStation 2", "15kHz 240p / 480i", "640x224" }
    };

    // Ask MAME for its own names once, so arcade entries list "Battle Garegga (Korea)"
    // rather than "bgaregga". Empty if MAME is not configured, in which case titles fall
    // back to the ROM stem.
    SetWindowText(hStaticStatus, L"Status: Reading MAME's game list...");
    std::wstring mameExeForScan = GetText(hEditMameExe);
    std::map<std::string, std::string> mameTitles = LoadMameTitles(mameExeForScan);

    // Ask MAME which of the arcade sets on disk will actually start, before listing any
    // of them. See FilterRunnableSets.
    std::set<std::string> runnable;
    {
        std::vector<std::string> stems;
        std::wstring mameRoms = GetText(hEditMameRoms);
        if (!mameRoms.empty() && fs::exists(mameRoms)) {
            try {
                for (const auto& e : fs::directory_iterator(mameRoms)) {
                    if (!e.is_regular_file()) continue;
                    auto ext = e.path().extension().string();
                    for (auto& c : ext) c = (char)tolower((unsigned char)c);
                    if (ext == ".zip" || ext == ".7z" || ext == ".chd") stems.push_back(e.path().stem().string());
                }
            } catch (...) {}
        }
        if (!stems.empty()) {
            SetWindowText(hStaticStatus, L"Status: Checking which sets MAME can run...");
            FilterRunnableSets(mameExeForScan, stems, runnable);
        }
    }
    int skipped = 0;

    std::wstring catPath = GetAppDir() + L"\\games_catalog.json";
    std::ofstream catOut; catOut.open(catPath.c_str());
    catOut << "{\n  \"games\": [\n";

    int totalFound = 0;

    for (const auto& target : targets) {
        if (target.path.empty() || !fs::exists(target.path)) continue;

        try {
            for (const auto& entry : fs::directory_iterator(target.path)) {
                if (entry.is_regular_file()) {
                    auto ext = entry.path().extension().string();
                    for (auto& c : ext) c = tolower(c);

                    if (ext == ".zip" || ext == ".7z" || ext == ".iso" || ext == ".chd" || 
                        ext == ".cso" || ext == ".elf" || ext == ".cue" || ext == ".sfc" || ext == ".md") {
                        std::string filename = entry.path().filename().string();
                        std::string stem = entry.path().stem().string();
                        // Leave out arcade sets MAME will not start - they would sit in
                        // the menu failing the moment anybody picked one.
                        if (target.system == "groovymame" && !runnable.empty() &&
                            runnable.find(stem) == runnable.end()) {
                            skipped++;
                            continue;
                        }

                        std::string id = target.system + "_" + stem;
                        std::string cleanRomPath = entry.path().generic_string();

                        // Arcade sets get MAME's own description, which carries the region.
                        // Everything else keeps the filename: Dolphin, PCSX2 and Flycast have
                        // no equivalent list to ask, and inventing titles for them would be
                        // guessing.
                        std::string title = stem;
                        if (target.system == "groovymame") {
                            auto it = mameTitles.find(stem);
                            if (it != mameTitles.end()) title = it->second;
                        }

                        std::wstring listEntry = L"[" + std::wstring(target.system.begin(), target.system.end()) + L"] " +
                                                StringToWstring(title);
                        SendMessage(hListGames, LB_ADDSTRING, 0, (LPARAM)listEntry.c_str());

                        if (totalFound > 0) catOut << ",\n";
                        catOut << "    {\n";
                        catOut << "      \"id\": \"" << id << "\",\n";
                        catOut << "      \"title\": \"" << JsonEscape(title) << "\",\n";
                        catOut << "      \"system\": \"" << target.system << "\",\n";
                        catOut << "      \"systemName\": \"" << target.systemName << "\",\n";
                        catOut << "      \"romName\": \"" << filename << "\",\n";
                        catOut << "      \"romPath\": \"" << cleanRomPath << "\",\n";
                        catOut << "      \"videoMode\": \"" << target.videoMode << "\",\n";
                        catOut << "      \"resolution\": \"" << target.resolution << "\"\n";
                        catOut << "    }";
                        totalFound++;
                    }
                }
            }
        } catch (...) {}
    }

    // No fallback catalog. This used to invent four arcade titles whenever a scan found
    // nothing, so a cabinet with no ROM paths configured still showed a menu full of games
    // it could not launch - every one of them failing at the moment somebody pressed the
    // button. An empty catalog is the truth, and the core renders it as a clear "library is
    // empty, run Auto-Scan" message rather than a dead list.

    catOut << "\n  ]\n}\n";
    catOut.close();

    std::wstring status = L"Status: Scanned " + std::to_wstring(totalFound) + L" game(s). Catalog saved.";
    if (skipped > 0) status += L" (" + std::to_wstring(skipped) + L" set(s) left out: MAME cannot run them.)";
    SetWindowText(hStaticStatus, status.c_str());
}

std::atomic<bool> g_launchInProgress(false);

// Internal helper that actually spawns GroovyMAME / RetroArch after delay
bool ExecuteLaunchProcess(const std::string& gameId, const std::wstring& targetMisterIp) {
    g_launchInProgress.store(false); // Reset guard once launched

    if (g_activePid > 0) {
        HANDLE hOld = OpenProcess(PROCESS_TERMINATE, FALSE, g_activePid);
        if (hOld) {
            TerminateProcess(hOld, 0);
            CloseHandle(hOld);
        }
        g_activePid = 0;
        Sleep(150);
    }

    std::wstring misterIp = targetMisterIp.empty() ? GetLearnedMisterIp() : targetMisterIp;
    if (misterIp.empty()) misterIp = L"192.168.1.50";
    int port = GetPort();

    std::wstring mameExe = GetText(hEditMameExe);
    std::wstring mameRoms = GetText(hEditMameRoms);

    std::string stem = gameId;
    if (stem.rfind("groovymame_", 0) == 0) {
        stem = stem.substr(11);
    }

    // Determine directory of MAME executable
    std::wstring mameDir = L"";
    size_t slashPos = mameExe.find_last_of(L"\\/");
    if (slashPos != std::wstring::npos) {
        mameDir = mameExe.substr(0, slashPos);
    }

    // If gameId indicates RetroArch
    if (gameId.rfind("retroarch_", 0) == 0) {
        std::wstring raExe = GetText(hEditRetroarchExe);
        std::wstring raRoms = GetText(hEditRetroarchRoms);
        std::string raStem = gameId.substr(10);
        std::wstring romFile = raRoms + L"\\" + StringToWstring(raStem) + L".zip";
        std::wstring cmd = L"\"" + raExe + L"\" -f \"" + romFile + L"\"";

        STARTUPINFO si = { sizeof(si) };
        PROCESS_INFORMATION pi = { 0 };
        std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
        cmdBuf.push_back(0);

        if (CreateProcessW(NULL, cmdBuf.data(), NULL, NULL, FALSE, CREATE_NEW_CONSOLE | NORMAL_PRIORITY_CLASS, NULL, NULL, &si, &pi)) {
            g_activePid = pi.dwProcessId;
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            SetWindowText(hStaticStatus, (L"Status: RetroArch launched (" + StringToWstring(raStem) + L")").c_str());
            return true;
        }
    }

    // -mister_ip is what tells GroovyMAME where to send the frames. Without it the game
    // starts on the PC, renders happily, and streams to nothing: the MiSTer sits on the
    // launcher and eventually gives up. It was dropped from here once as a "legacy
    // argument", which also broke launching MAME by hand for anyone whose mame.ini had
    // no mister_ip set.
    //
    // The address is the one the launch request arrived from, so a cabinet on DHCP needs
    // nothing typed in anywhere; the GUI field is only the fallback for a manual launch.
    std::wstring wStem = StringToWstring(stem);
    std::wstring misterArgs = L" -video mister -mister_ip " + misterIp + L" -skip_gameinfo -nokeepaspect";
    std::wstring cmd = L"\"" + mameExe + L"\" " + wStem + misterArgs;

    STARTUPINFO si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(0);

    BOOL ok = CreateProcessW(
        NULL,
        cmdBuf.data(),
        NULL,
        NULL,
        FALSE,
        CREATE_NEW_CONSOLE | NORMAL_PRIORITY_CLASS,
        NULL,
        mameDir.empty() ? NULL : mameDir.c_str(),
        &si,
        &pi
    );

    if (!ok) {
        // Fallback: try launching with just "mame" using system PATH
        std::wstring fallbackCmd = L"mame " + wStem + misterArgs;
        std::vector<wchar_t> fbBuf(fallbackCmd.begin(), fallbackCmd.end());
        fbBuf.push_back(0);
        ok = CreateProcessW(
            NULL,
            fbBuf.data(),
            NULL,
            NULL,
            FALSE,
            CREATE_NEW_CONSOLE | NORMAL_PRIORITY_CLASS,
            NULL,
            NULL,
            &si,
            &pi
        );
    }

    if (ok) {
        g_activePid = pi.dwProcessId;
        CloseHandle(pi.hThread);

        // Spawning successfully is not the same as running. MAME exits within a second
        // or two when it cannot find or run the set - a ROM that is missing, a parent
        // romset, or an entry that is not a playable game at all (plenty of things in a
        // roms folder are devices or unsupported drivers). That used to look exactly
        // like a core or network fault from the cabinet: the launcher sat on "starting
        // stream" and eventually gave up with nothing to say. This waits long enough to
        // catch that case and reports it.
        //
        // We are on the launch worker thread, so blocking here costs nothing.
        DWORD waited = WaitForSingleObject(pi.hProcess, 6000);
        if (waited == WAIT_OBJECT_0) {
            DWORD code = 0;
            GetExitCodeProcess(pi.hProcess, &code);
            CloseHandle(pi.hProcess);
            g_activePid = 0;
            std::wstring stat = L"Status: [FAILED] " + wStem + L" exited immediately (code " +
                                std::to_wstring(code) + L"). ROM missing, or not a playable set?";
            SetWindowText(hStaticStatus, stat.c_str());
            return false;
        }
        CloseHandle(pi.hProcess);

        std::wstring stat = L"Status: [RUNNING] GroovyMAME (" + wStem + L") -> Streaming to MiSTer (" + misterIp + L":" + std::to_wstring(port) + L")";
        SetWindowText(hStaticStatus, stat.c_str());
        return true;
    } else {
        DWORD err = GetLastError();
        std::wstring stat = L"Status: Could not launch MAME (Error " + std::to_wstring(err) + L"). Check MAME executable path!";
        SetWindowText(hStaticStatus, stat.c_str());
        return false;
    }
}

struct LaunchTaskParams {
    std::string gameId;
    std::wstring misterIp;
    int delaySec;
};

static DWORD WINAPI DelayedLaunchWorker(LPVOID lpParam) {
    LaunchTaskParams* params = (LaunchTaskParams*)lpParam;
    int delay = params->delaySec;
    std::string gid = params->gameId;
    std::wstring ip = params->misterIp;
    delete params;

    if (delay > 0) {
        for (int s = delay; s > 0; --s) {
            std::wstring st = L"Status: Launching PC stream in " + std::to_wstring(s) + L"s...";
            SetWindowText(hStaticStatus, st.c_str());
            Sleep(1000);
        }
    }

    SetWindowText(hStaticStatus, L"Status: Launching GroovyMAME stream to MiSTer GroovyNLC core...");
    ExecuteLaunchProcess(gid, ip);
    return 0;
}

// Public LaunchGame function: runs the launch on a worker thread, with an optional delay
bool LaunchGame(const std::string& gameId, const std::wstring& targetMisterIp) {
    static CRITICAL_SECTION cs;
    static bool init = false;
    if (!init) {
        InitializeCriticalSection(&cs);
        init = true;
    }

    EnterCriticalSection(&cs);
    if (g_launchInProgress.load()) {
        LeaveCriticalSection(&cs);
        SetWindowText(hStaticStatus, L"Status: Launch already in progress. Please wait...");
        return false;
    }
    g_launchInProgress.store(true);
    LeaveCriticalSection(&cs);

    // Default 0. The delay existed because the old flow switched the FPGA to Groovy.rbf at
    // launch time and the PC had to wait for reconfiguration. The merged core never switches:
    // it is already loaded, already listening, and sits on the launcher until video arrives.
    // The setting stays for anyone still driving this daemon from the old scripts.
    int delaySec = 0;
    if (hEditLaunchDelay != NULL) {
        std::wstring dStr = GetText(hEditLaunchDelay);
        if (!dStr.empty()) {
            try { delaySec = std::stoi(dStr); } catch (...) {}
        }
    }
    if (delaySec < 0) delaySec = 0;

    LaunchTaskParams* params = new LaunchTaskParams{ gameId, targetMisterIp, delaySec };
    HANDLE hThread = CreateThread(NULL, 0, DelayedLaunchWorker, params, 0, NULL);
    if (hThread) {
        CloseHandle(hThread);
        return true;
    }
    g_launchInProgress.store(false);
    return false;
}

// Background HTTP Worker Thread Function (Serves /catalog.json on TCP :8088)
DWORD WINAPI HttpThreadProc(LPVOID lpParam) {
    g_httpSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_httpSocket == INVALID_SOCKET) return 1;

    BOOL opt = TRUE;
    setsockopt(g_httpSocket, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    sockaddr_in bindAddr = { 0 };
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_port = htons(8088);
    bindAddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(g_httpSocket, (sockaddr*)&bindAddr, sizeof(bindAddr)) == SOCKET_ERROR) {
        closesocket(g_httpSocket);
        return 1;
    }

    listen(g_httpSocket, 5);

    while (g_daemonRunning) {
        sockaddr_in clientAddr;
        int clientLen = sizeof(clientAddr);
        SOCKET clientSock = accept(g_httpSocket, (sockaddr*)&clientAddr, &clientLen);
        if (clientSock == INVALID_SOCKET) break;

        char buf[2048];
        int r = recv(clientSock, buf, sizeof(buf) - 1, 0);
        if (r > 0) {
            buf[r] = 0;
            std::string req(buf);
            if (req.find("GET /catalog.json") != std::string::npos || req.find("GET / ") != std::string::npos) {
                std::wstring catPath = GetAppDir() + L"\\games_catalog.json";
                std::ifstream f; f.open(catPath.c_str());
                std::string body = "";
                if (f.is_open()) {
                    std::stringstream ss;
                    ss << f.rdbuf();
                    body = ss.str();
                } else {
                    body = "{\"games\":[]}";
                }

                std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: " + 
                                   std::to_string(body.length()) + "\r\nConnection: close\r\n\r\n" + body;
                send(clientSock, resp.c_str(), (int)resp.length(), 0);
            } else {
                std::string notFound = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
                send(clientSock, notFound.c_str(), (int)notFound.length(), 0);
            }
        }
        closesocket(clientSock);
    }
    closesocket(g_httpSocket);
    return 0;
}



DWORD WINAPI DaemonThreadProc(LPVOID lpParam) {
    int port = g_configuredPort.load();

    g_udpSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_udpSocket == INVALID_SOCKET) {
        SetWindowText(hStaticStatus, L"Error: Failed to create UDP socket");
        return 1;
    }

    BOOL opt = TRUE;
    setsockopt(g_udpSocket, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    sockaddr_in serverAddr = { 0 };
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);
    serverAddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(g_udpSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        std::wstring err = L"Error: Could not bind to UDP port " + std::to_wstring(port) + L" (Socket Error: " + std::to_wstring(WSAGetLastError()) + L")";
        SetWindowText(hStaticStatus, err.c_str());
        closesocket(g_udpSocket);
        return 1;
    }

    char buffer[4096];
    sockaddr_in clientAddr;
    int clientLen = sizeof(clientAddr);

    while (g_daemonRunning) {
        int bytes = recvfrom(g_udpSocket, buffer, sizeof(buffer) - 1, 0, (sockaddr*)&clientAddr, &clientLen);
        if (bytes > 0) {
            buffer[bytes] = '\0';
            std::string msg(buffer);

            // Auto-detect MiSTer client IP address from UDP packet
            char clientIpStr[INET_ADDRSTRLEN] = { 0 };
            inet_ntop(AF_INET, &(clientAddr.sin_addr), clientIpStr, INET_ADDRSTRLEN);
            std::wstring misterClientIp = StringToWstring(clientIpStr);

            // Every datagram from a cabinet re-confirms where it is. This is the only
            // source of the address: there is no field to type it into, and a MiSTer
            // that moves to a new DHCP lease is picked up on its next broadcast.
            NoteMisterAddress(misterClientIp);

            if (msg.rfind("DISCOVER_PHANTOM", 0) == 0) {
                std::string reply = "PHANTOM_HOST_ONLINE:" + std::to_string(port) + ":8088";
                sendto(g_udpSocket, reply.c_str(), (int)reply.length(), 0, (sockaddr*)&clientAddr, clientLen);
            } else if (msg == "GET_CATALOG") {
                std::wstring catPath = GetAppDir() + L"\\games_catalog.json";
                std::ifstream f; f.open(catPath.c_str());
                std::string catData = "";
                if (f.is_open()) {
                    std::stringstream ss;
                    ss << f.rdbuf();
                    catData = ss.str();
                }
                if (catData.empty()) {
                    catData = "{\"games\":[]}";
                }
                int sendLen = (int)std::min(catData.length(), (size_t)60000);
                sendto(g_udpSocket, catData.c_str(), sendLen, 0, (sockaddr*)&clientAddr, clientLen);
            } else if (msg.rfind("LAUNCH:", 0) == 0) {
                // MiSTer requested emulator game launch!
                std::string gameId = msg.substr(7);
                while (!gameId.empty() && (gameId.back() == '\r' || gameId.back() == '\n' || gameId.back() == ' ')) {
                    gameId.pop_back();
                }

                size_t dPos = gameId.find(":delay=");
                if (dPos != std::string::npos) {
                    gameId = gameId.substr(0, dPos);
                }

                // Call LaunchGame directly — LaunchGame handles the single unified delayed worker thread safely
                LaunchGame(gameId, misterClientIp);

                std::string reply = "ACK:LAUNCH:OK:" + gameId;
                sendto(g_udpSocket, reply.c_str(), (int)reply.length(), 0, (sockaddr*)&clientAddr, clientLen);
            } else if (msg == "KILL") {
                if (g_activePid > 0) {
                    HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, g_activePid);
                    if (hProc) {
                        TerminateProcess(hProc, 0);
                        CloseHandle(hProc);
                    }
                    g_activePid = 0;
                }
                const char* reply = "ACK:KILL:OK";
                sendto(g_udpSocket, reply, (int)strlen(reply), 0, (sockaddr*)&clientAddr, clientLen);
            } else if (msg == "PING") {
                const char* reply = "PONG";
                sendto(g_udpSocket, reply, (int)strlen(reply), 0, (sockaddr*)&clientAddr, clientLen);
            }
        }
    }

    closesocket(g_udpSocket);
    return 0;
}

// Ensure Windows Firewall permits UDP & HTTP
void EnsureFirewallRules() {
    wchar_t exePath[MAX_PATH];
    GetModuleFileName(NULL, exePath, MAX_PATH);

    std::wstring cmd = L"advfirewall firewall add rule name=\"Phantom Arcade Manager\" dir=in action=allow program=\"" + 
                       std::wstring(exePath) + L"\" enable=yes profile=any";
    ShellExecute(NULL, L"runas", L"netsh", cmd.c_str(), NULL, SW_HIDE);
}

void ToggleDaemon() {
    if (!g_daemonRunning) {
        int port = GetPort();
        g_configuredPort.store(port);
        g_daemonRunning = true;

        WSADATA wsaData;
        WSAStartup(MAKEWORD(2, 2), &wsaData);

        g_hDaemonThread = CreateThread(NULL, 0, DaemonThreadProc, NULL, 0, NULL);
        g_hHttpThread = CreateThread(NULL, 0, HttpThreadProc, NULL, 0, NULL);

        SetWindowText(hBtnToggleDaemon, L"Stop Background Daemon");
        std::wstring status = L"Status: Background Daemon RUNNING on UDP :" + std::to_wstring(port) + L" & HTTP :8088. Waiting for MiSTer...";
        SetWindowText(hStaticStatus, status.c_str());
    } else {
        g_daemonRunning = false;
        if (g_udpSocket != INVALID_SOCKET) {
            closesocket(g_udpSocket);
            g_udpSocket = INVALID_SOCKET;
        }
        if (g_httpSocket != INVALID_SOCKET) {
            closesocket(g_httpSocket);
            g_httpSocket = INVALID_SOCKET;
        }
        if (g_hDaemonThread) {
            WaitForSingleObject(g_hDaemonThread, 1000);
            CloseHandle(g_hDaemonThread);
            g_hDaemonThread = NULL;
        }
        if (g_hHttpThread) {
            WaitForSingleObject(g_hHttpThread, 1000);
            CloseHandle(g_hHttpThread);
            g_hHttpThread = NULL;
        }
        SetWindowText(hBtnToggleDaemon, L"Start Background Daemon");
        SetWindowText(hStaticStatus, L"Status: Daemon Stopped");
    }
}

// Window Procedure
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        int y = 12;
        CreateWindow(L"STATIC", L"MiSTer:", WS_CHILD | WS_VISIBLE, 20, y, 55, 20, hWnd, NULL, hInst, NULL);
        hStaticMisterIp = CreateWindow(L"STATIC", L"waiting for a cabinet to announce itself...",
                                      WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, 80, y + 2, 280, 20,
                                      hWnd, NULL, hInst, NULL);

        CreateWindow(L"STATIC", L"Launch Delay (s):", WS_CHILD | WS_VISIBLE, 365, y, 110, 20, hWnd, NULL, hInst, NULL);
        hEditLaunchDelay = CreateWindow(L"EDIT", L"0", WS_CHILD | WS_VISIBLE | WS_BORDER, 480, y, 40, 22, hWnd, (HMENU)IDC_EDIT_LAUNCH_DELAY, hInst, NULL);

        // 1. GroovyMAME
        y += 32;
        CreateWindow(L"STATIC", L"GroovyMAME Executable:", WS_CHILD | WS_VISIBLE, 20, y, 170, 20, hWnd, NULL, hInst, NULL);
        hEditMameExe = CreateWindow(L"EDIT", L"C:\\Emulators\\GroovyMAME\\groovymame64.exe", WS_CHILD | WS_VISIBLE | WS_BORDER, 195, y, 350, 22, hWnd, (HMENU)IDC_EDIT_GROOVYMAME_EXE, hInst, NULL);
        CreateWindow(L"BUTTON", L"Browse...", WS_CHILD | WS_VISIBLE, 555, y, 85, 22, hWnd, (HMENU)IDC_BTN_BROWSE_MAME_EXE, hInst, NULL);

        y += 26;
        CreateWindow(L"STATIC", L"GroovyMAME ROMs Folder:", WS_CHILD | WS_VISIBLE, 20, y, 170, 20, hWnd, NULL, hInst, NULL);
        hEditMameRoms = CreateWindow(L"EDIT", L"C:\\Emulators\\GroovyMAME\\roms", WS_CHILD | WS_VISIBLE | WS_BORDER, 195, y, 350, 22, hWnd, (HMENU)IDC_EDIT_MAME_ROMS, hInst, NULL);
        CreateWindow(L"BUTTON", L"Browse...", WS_CHILD | WS_VISIBLE, 555, y, 85, 22, hWnd, (HMENU)IDC_BTN_BROWSE_MAME_ROMS, hInst, NULL);

        // 2. RetroArch
        y += 32;
        CreateWindow(L"STATIC", L"RetroArch Executable:", WS_CHILD | WS_VISIBLE, 20, y, 170, 20, hWnd, NULL, hInst, NULL);
        hEditRetroarchExe = CreateWindow(L"EDIT", L"C:\\Emulators\\RetroArch\\retroarch.exe", WS_CHILD | WS_VISIBLE | WS_BORDER, 195, y, 350, 22, hWnd, (HMENU)IDC_EDIT_RETROARCH_EXE, hInst, NULL);
        CreateWindow(L"BUTTON", L"Browse...", WS_CHILD | WS_VISIBLE, 555, y, 85, 22, hWnd, (HMENU)IDC_BTN_BROWSE_RA_EXE, hInst, NULL);

        y += 26;
        CreateWindow(L"STATIC", L"RetroArch ROMs Folder:", WS_CHILD | WS_VISIBLE, 20, y, 170, 20, hWnd, NULL, hInst, NULL);
        hEditRetroarchRoms = CreateWindow(L"EDIT", L"C:\\Games\\RetroArch\\roms", WS_CHILD | WS_VISIBLE | WS_BORDER, 195, y, 350, 22, hWnd, (HMENU)IDC_EDIT_RETROARCH_ROMS, hInst, NULL);
        CreateWindow(L"BUTTON", L"Browse...", WS_CHILD | WS_VISIBLE, 555, y, 85, 22, hWnd, (HMENU)IDC_BTN_BROWSE_RA_ROMS, hInst, NULL);

        // 3. Dolphin
        y += 32;
        CreateWindow(L"STATIC", L"Dolphin Executable:", WS_CHILD | WS_VISIBLE, 20, y, 170, 20, hWnd, NULL, hInst, NULL);
        hEditDolphinExe = CreateWindow(L"EDIT", L"C:\\Emulators\\Dolphin\\Dolphin.exe", WS_CHILD | WS_VISIBLE | WS_BORDER, 195, y, 350, 22, hWnd, (HMENU)IDC_EDIT_DOLPHIN_EXE, hInst, NULL);
        CreateWindow(L"BUTTON", L"Browse...", WS_CHILD | WS_VISIBLE, 555, y, 85, 22, hWnd, (HMENU)IDC_BTN_BROWSE_DOLPHIN, hInst, NULL);

        y += 26;
        CreateWindow(L"STATIC", L"GameCube/Wii ROMs:", WS_CHILD | WS_VISIBLE, 20, y, 170, 20, hWnd, NULL, hInst, NULL);
        hEditGcRoms = CreateWindow(L"EDIT", L"C:\\Games\\GameCube", WS_CHILD | WS_VISIBLE | WS_BORDER, 195, y, 350, 22, hWnd, (HMENU)IDC_EDIT_GC_ROMS, hInst, NULL);
        CreateWindow(L"BUTTON", L"Browse...", WS_CHILD | WS_VISIBLE, 555, y, 85, 22, hWnd, (HMENU)IDC_BTN_BROWSE_GCROMS, hInst, NULL);

        // 4. Flycast
        y += 32;
        CreateWindow(L"STATIC", L"Flycast Executable:", WS_CHILD | WS_VISIBLE, 20, y, 170, 20, hWnd, NULL, hInst, NULL);
        hEditFlycastExe = CreateWindow(L"EDIT", L"C:\\Emulators\\Flycast\\flycast.exe", WS_CHILD | WS_VISIBLE | WS_BORDER, 195, y, 350, 22, hWnd, (HMENU)IDC_EDIT_FLYCAST_EXE, hInst, NULL);
        CreateWindow(L"BUTTON", L"Browse...", WS_CHILD | WS_VISIBLE, 555, y, 85, 22, hWnd, (HMENU)IDC_BTN_BROWSE_FLYCAST, hInst, NULL);

        y += 26;
        CreateWindow(L"STATIC", L"Naomi/Arcade ROMs:", WS_CHILD | WS_VISIBLE, 20, y, 170, 20, hWnd, NULL, hInst, NULL);
        hEditNaomiRoms = CreateWindow(L"EDIT", L"C:\\Games\\Arcade\\Naomi", WS_CHILD | WS_VISIBLE | WS_BORDER, 195, y, 350, 22, hWnd, (HMENU)IDC_EDIT_NAOMI_ROMS, hInst, NULL);
        CreateWindow(L"BUTTON", L"Browse...", WS_CHILD | WS_VISIBLE, 555, y, 85, 22, hWnd, (HMENU)IDC_BTN_BROWSE_NAOMI, hInst, NULL);

        // 5. PCSX2
        y += 32;
        CreateWindow(L"STATIC", L"PCSX2 Executable (Opt):", WS_CHILD | WS_VISIBLE, 20, y, 170, 20, hWnd, NULL, hInst, NULL);
        hEditPcsx2Exe = CreateWindow(L"EDIT", L"C:\\Emulators\\PCSX2\\pcsx2-qt.exe", WS_CHILD | WS_VISIBLE | WS_BORDER, 195, y, 350, 22, hWnd, (HMENU)IDC_EDIT_PCSX2_EXE, hInst, NULL);
        CreateWindow(L"BUTTON", L"Browse...", WS_CHILD | WS_VISIBLE, 555, y, 85, 22, hWnd, (HMENU)IDC_BTN_BROWSE_PCSX2, hInst, NULL);

        y += 26;
        CreateWindow(L"STATIC", L"PS2 ROMs Folder:", WS_CHILD | WS_VISIBLE, 20, y, 170, 20, hWnd, NULL, hInst, NULL);
        hEditPs2Roms = CreateWindow(L"EDIT", L"C:\\Games\\PS2", WS_CHILD | WS_VISIBLE | WS_BORDER, 195, y, 350, 22, hWnd, (HMENU)IDC_EDIT_PS2_ROMS, hInst, NULL);
        CreateWindow(L"BUTTON", L"Browse...", WS_CHILD | WS_VISIBLE, 555, y, 85, 22, hWnd, (HMENU)IDC_BTN_BROWSE_PS2ROMS, hInst, NULL);

        // Action Buttons Row
        y += 36;
        CreateWindow(L"BUTTON", L"1. Scan ROMs", WS_CHILD | WS_VISIBLE, 20, y, 110, 28, hWnd, (HMENU)IDC_BTN_SCAN_ROMS, hInst, NULL);
        CreateWindow(L"BUTTON", L"2. Save Settings", WS_CHILD | WS_VISIBLE, 140, y, 120, 28, hWnd, (HMENU)IDC_BTN_SAVE_CONFIG, hInst, NULL);
        hBtnLaunchGame = CreateWindow(L"BUTTON", L"▶ Launch Game", WS_CHILD | WS_VISIBLE, 270, y, 130, 28, hWnd, (HMENU)IDC_BTN_LAUNCH_GAME, hInst, NULL);
        hBtnToggleDaemon = CreateWindow(L"BUTTON", L"Start Background Daemon", WS_CHILD | WS_VISIBLE, 410, y, 230, 28, hWnd, (HMENU)IDC_BTN_TOGGLE_DAEMON, hInst, NULL);

        // Scanned ROMs List Box
        y += 36;
        CreateWindow(L"STATIC", L"Games Catalog (Double-click or press Launch to test CRT stream):", WS_CHILD | WS_VISIBLE, 20, y, 500, 18, hWnd, NULL, hInst, NULL);
        y += 18;
        hListGames = CreateWindow(L"LISTBOX", NULL, WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | LBS_NOTIFY, 20, y, 620, 120, hWnd, (HMENU)IDC_LIST_GAMES, hInst, NULL);

        // Status Bar
        y += 128;
        hStaticStatus = CreateWindow(L"STATIC", L"Status: Ready. Default MiSTer port is 1999.", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, 20, y, 620, 20, hWnd, (HMENU)IDC_STATIC_STATUS, hInst, NULL);

        LoadConfiguration();
        break;
    }

    case WM_COMMAND: {
        int wmId = LOWORD(wParam);
        int event = HIWORD(wParam);

        if (wmId == IDC_LIST_GAMES && event == LBN_DBLCLK) {
            // Double-click to launch game directly
            int sel = (int)SendMessage(hListGames, LB_GETCURSEL, 0, 0);
            if (sel != LB_ERR) {
                wchar_t itemText[256] = { 0 };
                SendMessage(hListGames, LB_GETTEXT, sel, (LPARAM)itemText);
                std::wstring s(itemText);
                size_t bracketEnd = s.find(L"] ");
                if (bracketEnd != std::wstring::npos) {
                    std::wstring title = s.substr(bracketEnd + 2);
                    std::string gid(title.begin(), title.end());
                    LaunchGame(gid);
                }
            }
            break;
        }

        switch (wmId) {
        case IDC_BTN_BROWSE_MAME_EXE: {
            auto path = BrowseFile(hWnd, L"GroovyMAME Executable (*.exe)\0*.exe\0All Files (*.*)\0*.*\0");
            if (!path.empty()) {
                SetWindowText(hEditMameExe, path.c_str());
                SaveConfiguration();
            }
            break;
        }
        case IDC_BTN_BROWSE_MAME_ROMS: {
            auto path = BrowseFolder(hWnd, L"Select GroovyMAME ROMs Folder");
            if (!path.empty()) {
                SetWindowText(hEditMameRoms, path.c_str());
                SaveConfiguration();
            }
            break;
        }
        case IDC_BTN_BROWSE_RA_EXE: {
            auto path = BrowseFile(hWnd, L"RetroArch Executable (retroarch.exe)\0retroarch.exe;*.exe\0All Files (*.*)\0*.*\0");
            if (!path.empty()) {
                SetWindowText(hEditRetroarchExe, path.c_str());
                SaveConfiguration();
            }
            break;
        }
        case IDC_BTN_BROWSE_RA_ROMS: {
            auto path = BrowseFolder(hWnd, L"Select RetroArch ROMs Folder");
            if (!path.empty()) {
                SetWindowText(hEditRetroarchRoms, path.c_str());
                SaveConfiguration();
            }
            break;
        }
        case IDC_BTN_BROWSE_DOLPHIN: {
            auto path = BrowseFile(hWnd, L"Dolphin Executable (*.exe)\0*.exe\0All Files (*.*)\0*.*\0");
            if (!path.empty()) {
                SetWindowText(hEditDolphinExe, path.c_str());
                SaveConfiguration();
            }
            break;
        }
        case IDC_BTN_BROWSE_GCROMS: {
            auto path = BrowseFolder(hWnd, L"Select GameCube/Wii ROMs Folder");
            if (!path.empty()) {
                SetWindowText(hEditGcRoms, path.c_str());
                SaveConfiguration();
            }
            break;
        }
        case IDC_BTN_BROWSE_FLYCAST: {
            auto path = BrowseFile(hWnd, L"Flycast Executable (*.exe)\0*.exe\0All Files (*.*)\0*.*\0");
            if (!path.empty()) {
                SetWindowText(hEditFlycastExe, path.c_str());
                SaveConfiguration();
            }
            break;
        }
        case IDC_BTN_BROWSE_NAOMI: {
            auto path = BrowseFolder(hWnd, L"Select Naomi/Arcade ROMs Folder");
            if (!path.empty()) {
                SetWindowText(hEditNaomiRoms, path.c_str());
                SaveConfiguration();
            }
            break;
        }
        case IDC_BTN_BROWSE_PCSX2: {
            auto path = BrowseFile(hWnd, L"PCSX2 Executable (*.exe)\0*.exe\0All Files (*.*)\0*.*\0");
            if (!path.empty()) {
                SetWindowText(hEditPcsx2Exe, path.c_str());
                SaveConfiguration();
            }
            break;
        }
        case IDC_BTN_BROWSE_PS2ROMS: {
            auto path = BrowseFolder(hWnd, L"Select PS2 ROMs Folder");
            if (!path.empty()) {
                SetWindowText(hEditPs2Roms, path.c_str());
                SaveConfiguration();
            }
            break;
        }
        case IDC_BTN_SAVE_CONFIG:
            SaveConfiguration();
            break;
        case IDC_BTN_SCAN_ROMS:
            ScanRomDirectories();
            break;
        case IDC_BTN_LAUNCH_GAME: {
            int sel = (int)SendMessage(hListGames, LB_GETCURSEL, 0, 0);
            std::string gid = "kinst";
            if (sel != LB_ERR) {
                wchar_t itemText[256] = { 0 };
                SendMessage(hListGames, LB_GETTEXT, sel, (LPARAM)itemText);
                std::wstring s(itemText);
                size_t bracketEnd = s.find(L"] ");
                if (bracketEnd != std::wstring::npos) {
                    std::wstring title = s.substr(bracketEnd + 2);
                    gid = std::string(title.begin(), title.end());
                }
            }
            LaunchGame(gid);
            break;
        }
        case IDC_BTN_TOGGLE_DAEMON:
            ToggleDaemon();
            break;
        }
        break;
    }

    case WM_DESTROY:
        if (g_daemonRunning) {
            ToggleDaemon();
        }
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProc(hWnd, msg, wParam, lParam);
    }
    return 0;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR lpCmdLine, int nCmdShow) {
    hInst = hInstance;

    const wchar_t CLASS_NAME[] = L"PhantomArcadeManagerWindowClass";

    WNDCLASS wc = { 0 };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);

    RegisterClass(&wc);

    hMainWnd = CreateWindowEx(
        0, CLASS_NAME, L"Phantom Arcade - Groovy_MiSTer Windows Setup & Launcher Daemon",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 680, 800,
        NULL, NULL, hInstance, NULL
    );

    if (hMainWnd == NULL) return 0;

    ShowWindow(hMainWnd, nCmdShow);
    UpdateWindow(hMainWnd);

    // Initial scan and start daemon immediately
    EnsureFirewallRules();
    ScanRomDirectories();
    ToggleDaemon();

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return (int)msg.wParam;
}
