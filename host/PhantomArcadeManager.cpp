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
// windows.h defines min and max as macros unless this is set, which turns the std::min
// below into "std::((a) < (b) ? (a) : (b))" and fails to compile.
#define NOMINMAX
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
#include "emulator_status.h"
#include "fbneo_sets.h"
#include "game_index.h"
#include "library_scan.h"

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
#define IDC_BTN_SCAN_ROMS           123
#define IDC_BTN_TEST_MISTER         124
#define IDC_BTN_SAVE_CONFIG         125
#define IDC_BTN_TOGGLE_DAEMON       126
#define IDC_STATIC_STATUS           127
#define IDC_LIST_GAMES              128
#define IDC_BTN_LAUNCH_GAME         129

// Global State
HINSTANCE hInst = NULL;
HWND hMainWnd = NULL;
HWND hStaticMisterIp = NULL;   // read-only: the address we discovered
HWND hStaticStatus, hListGames;
HWND hBtnToggleDaemon, hBtnLaunchGame;

// ---- the emulators -----------------------------------------------------------------
//
// One row here is one emulator: its UI, its config keys, its scan pass and its launch
// command all come from this table, so adding or removing one is a single edit rather
// than the same change made in six places.
//
// The set is the GroovyNLC-capable emulators from https://github.com/verbst/repositories,
// plus GroovyMAME, which is Calamity's and is where the arcade support comes from, plus
// Dolphin and 86Box, whose GroovyNLC support is ours (retrorepair/dolphin and
// retrorepair/86Box, branch groovy-nlc).
//
// Only GroovyMAME takes the MiSTer's address on the command line. The rest are configured
// once in their own settings - xemu has Settings > MiSTer, RetroArch has Settings > Groovy
// MiSTer, rpcs3 and pcsx2 have their own MiSTer pages, Dolphin and 86Box have a
// [GroovyMiSTer] section in Dolphin.ini and 86box.cfg - so the launcher only has to start
// them with the right file.
//
// args placeholders: {rom} full path, {rom_stem} bare name, {rom_dir} containing folder,
// {mister_ip} discovered address.
struct EmulatorDef {
    const char*    key;       // catalog "system" value, and the config key prefix
    const wchar_t* label;     // UI label
    const char*    sysName;   // SYSTEM column on the cabinet
    const wchar_t* defExe;
    const wchar_t* defRoms;
    const wchar_t* exts;      // lower-case, dot-prefixed, comma separated
    const wchar_t* defArgs;
    const char*    videoMode; // fallback only; MAME sets its own from -listxml
    HWND           exeEdit;
    HWND           romsEdit;
};

static EmulatorDef g_emus[] = {
    { "groovymame", L"GroovyMAME", "GroovyMAME Arcade",
      L"C:\\Emulators\\GroovyMAME\\groovymame64.exe", L"C:\\Emulators\\GroovyMAME\\roms",
      L".zip,.7z,.chd",
      L"{rom_stem} -video mister -mister_ip {mister_ip} -joystickprovider mister -skip_gameinfo -nokeepaspect",
      "15kHz Native", NULL, NULL },

    { "fbneo", L"FBNeo (Fightcade)", "FBNeo Arcade",
      L"C:\\Emulators\\fbneo\\fcadefbneo.exe", L"C:\\Emulators\\fbneo\\ROMs",
      L".zip,.7z",
      // FBNeo takes a game's short name, not a path: its command line is compared with each
      // driver's name for an exact match, so a ROM path matches nothing and it stops with a
      // "not supported" error. It finds the zip itself, in its ROM folder.
      L"{rom_stem}",
      "15kHz Native", NULL, NULL },

    { "flycast", L"Flycast (Dojo)", "Sega Dreamcast / NAOMI",
      L"C:\\Emulators\\flycast\\flycast.exe", L"C:\\Games\\Naomi",
      L".zip,.7z,.chd,.gdi,.cdi,.cue,.lst",
      L"\"{rom}\"",
      "15kHz 240p / 480i", NULL, NULL },

    { "pcsx2", L"PCSX2", "Sony PlayStation 2",
      L"C:\\Emulators\\pcsx2\\pcsx2-qtx64.exe", L"C:\\Games\\PS2",
      L".iso,.chd,.cso,.gz,.bin",
      L"-batch -nogui \"{rom}\"",
      "15kHz 240p / 480i", NULL, NULL },

    { "rpcs3", L"RPCS3", "Sony PlayStation 3",
      L"C:\\Emulators\\rpcs3\\rpcs3.exe", L"C:\\Games\\PS3",
      L".iso,.pkg,.self,.elf,.bin",
      L"--no-gui \"{rom}\"",
      "480p via MiSTer settings", NULL, NULL },

    { "xemu", L"xemu (Xbox)", "Microsoft Xbox",
      L"C:\\Emulators\\xemu\\xemu.exe", L"C:\\Games\\Xbox",
      L".iso,.xiso",
      L"-dvd_path \"{rom}\"",
      "15kHz 480i", NULL, NULL },

    { "dolphin", L"Dolphin", "Nintendo GameCube / Wii",
      L"C:\\Emulators\\dolphin\\Dolphin.exe", L"C:\\Games\\GameCube",
      L".iso,.gcm,.gcz,.rvz,.wia,.ciso,.wbfs,.wad,.dol,.elf,.m3u",
      L"-b -e \"{rom}\"",
      "15kHz 480i / 31kHz 480p", NULL, NULL },

    // 86Box is per-machine rather than per-ROM: a VM is a folder holding 86box.cfg and its
    // disk images, and -P points at the folder. The scan matches the config file because
    // that is the one file every VM has, and {rom_dir} turns it back into the folder.
    { "86box", L"86Box (PC)", "PC / 3dfx Voodoo",
      L"C:\\Emulators\\86box\\86Box.exe", L"C:\\Emulators\\86box\\vms",
      L".cfg",
      L"-P \"{rom_dir}\"",
      "15kHz 15/25/31kHz VGA", NULL, NULL },

    { "retroarch", L"RetroArch", "RetroArch",
      L"C:\\Emulators\\RetroArch\\retroarch.exe", L"C:\\Games\\RetroArch",
      L".zip,.7z,.chd,.cue,.bin,.sfc,.smc,.md,.gen,.nes,.pce,.gg,.sms,.n64,.z64",
      L"\"{rom}\"",
      "15kHz Dynamic", NULL, NULL },
};
static const int g_emuCount = (int)(sizeof(g_emus) / sizeof(g_emus[0]));

// Control IDs are allocated from a base so the table stays the only place a new
// emulator has to be mentioned.
#define IDC_EMU_BASE 400
#define IDC_EMU_EXE(i)        (IDC_EMU_BASE + (i) * 4 + 0)
#define IDC_EMU_EXE_BROWSE(i) (IDC_EMU_BASE + (i) * 4 + 1)
#define IDC_EMU_ROMS(i)       (IDC_EMU_BASE + (i) * 4 + 2)
#define IDC_EMU_ROMS_BROWSE(i)(IDC_EMU_BASE + (i) * 4 + 3)

static EmulatorDef* EmuByKey(const std::string& key) {
    for (int i = 0; i < g_emuCount; i++) if (key == g_emus[i].key) return &g_emus[i];
    return NULL;
}

// Config keys used before the table existed, so an upgrade keeps the paths somebody
// already set. Dropping this silently reset GroovyMAME to an empty default folder and
// the arcade games simply disappeared from the cabinet.
static const char* LegacyKeyFor(const char* key) {
    if (!strcmp(key, "groovymame")) return "mame";
    return NULL;
}

std::atomic<bool> g_daemonRunning(false);
std::atomic<DWORD> g_activePid(0);
std::atomic<ULONGLONG> g_activeStart(0);   // that process's creation time, so a recycled pid is not mistaken for it
std::atomic<int> g_configuredPort(1999);
SOCKET g_udpSocket = INVALID_SOCKET;
SOCKET g_httpSocket = INVALID_SOCKET;
HANDLE g_hDaemonThread = NULL;
HANDLE g_hHttpThread = NULL;

// Forward declarations
void LoadConfiguration();
void SaveConfiguration();
void StartLibraryScan(bool full);
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

// The extension lists in g_emus are plain ASCII, so a narrowing copy is enough.
static std::string WstringToString(const std::wstring& w) {
    std::string out;
    out.reserve(w.size());
    for (wchar_t c : w) out.push_back((c < 128) ? (char)c : '?');
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
    // Something on this PC talking to us (a test client, a second tool) is not a cabinet, and must not
    // overwrite the address of the real one.
    if (ip.rfind(L"127.", 0) == 0) return;
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

    // 3. Emulator paths, straight off the table.
    for (int i = 0; i < g_emuCount; i++) {
        EmulatorDef& e = g_emus[i];
        std::string kExe  = std::string(e.key) + "_exe";
        std::string kRoms = std::string(e.key) + "_roms";

        const char* legacy = LegacyKeyFor(e.key);
        std::string lExe  = legacy ? std::string(legacy) + "_exe"  : "";
        std::string lRoms = legacy ? std::string(legacy) + "_roms" : "";

        std::string jExe = ExtractJsonString(json, kExe.c_str());
        if (jExe.empty() && legacy) jExe = ExtractJsonString(json, lExe.c_str());
        std::wstring wExe = jExe.empty()
            ? RegReadString(StringToWstring(kExe).c_str(),
                            legacy ? RegReadString(StringToWstring(lExe).c_str(), e.defExe).c_str()
                                   : e.defExe)
            : StringToWstring(jExe);
        SetWindowText(e.exeEdit, wExe.c_str());

        std::string jRoms = ExtractJsonString(json, kRoms.c_str());
        if (jRoms.empty() && legacy) jRoms = ExtractJsonString(json, lRoms.c_str());
        std::wstring wRoms = jRoms.empty()
            ? RegReadString(StringToWstring(kRoms).c_str(),
                            legacy ? RegReadString(StringToWstring(lRoms).c_str(), e.defRoms).c_str()
                                   : e.defRoms)
            : StringToWstring(jRoms);
        SetWindowText(e.romsEdit, wRoms.c_str());
    }

    SetWindowText(hStaticStatus, L"Status: Configuration loaded from disk and registry.");
}

// Save Configuration to phantom_config.json AND Windows Registry
void SaveConfiguration() {
    std::wstring misterIp = GetLearnedMisterIp();
    int port = GetPort();
    // Everything emulator-shaped comes off the table, so a new one needs no change here.
    std::wstring cfgPath = GetConfigPath();
    std::ofstream out; out.open(cfgPath.c_str());

    RegWriteString(L"mister_client_ip", misterIp);
    RegWriteInt(L"udp_port", port);
    for (int i = 0; i < g_emuCount; i++) {
        EmulatorDef& e = g_emus[i];
        RegWriteString(StringToWstring(std::string(e.key) + "_exe").c_str(),  GetText(e.exeEdit).c_str());
        RegWriteString(StringToWstring(std::string(e.key) + "_roms").c_str(), GetText(e.romsEdit).c_str());
    }

    if (out.is_open()) {
        out << "{\n";
        out << "  \"server\": {\n";
        out << "    \"listen_ip\": \"0.0.0.0\",\n";
        out << "    \"udp_port\": " << port << ",\n";
        out << "    \"http_port\": 8088,\n";
        out << "    \"mister_client_ip\": \"" << ToJsonString(misterIp) << "\"\n";
        out << "  },\n";
        out << "  \"paths\": {\n";
        for (int i = 0; i < g_emuCount; i++) {
            EmulatorDef& e = g_emus[i];
            out << "    \"" << e.key << "_exe\": \""  << ToJsonString(GetText(e.exeEdit))  << "\",\n";
            out << "    \"" << e.key << "_roms\": \"" << ToJsonString(GetText(e.romsEdit)) << "\""
                << (i + 1 < g_emuCount ? "," : "") << "\n";
        }
        out << "  },\n";
        out << "  \"emulators\": {\n";
        for (int i = 0; i < g_emuCount; i++) {
            EmulatorDef& e = g_emus[i];
            out << "    \"" << e.key << "\": {\n";
            out << "      \"exe\": \""  << ToJsonString(GetText(e.exeEdit))  << "\",\n";
            out << "      \"roms\": \"" << ToJsonString(GetText(e.romsEdit)) << "\",\n";
            out << "      \"args\": \"" << ToJsonString(e.defArgs) << "\"\n";
            out << "    }" << (i + 1 < g_emuCount ? "," : "") << "\n";
        }
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
// What MAME knows about a set beyond its name. All of it comes out of the same -listxml
// pass that decides runnability, so it costs nothing extra.
struct MameMeta {
    std::string year;
    std::string manufacturer;
    int width = 0, height = 0, rotate = 0;
    double refresh = 0.0;
};

// Pull one <tag>text</tag> out of a bounded slice.
static std::string XmlTag(const std::string& block, const char* tag) {
    std::string open = std::string("<") + tag + ">";
    std::string close = std::string("</") + tag + ">";
    size_t a = block.find(open);
    if (a == std::string::npos) return "";
    size_t b = block.find(close, a);
    if (b == std::string::npos) return "";
    std::string v = block.substr(a + open.size(), b - a - open.size());
    // the few entities MAME emits in these fields
    for (size_t p; (p = v.find("&apos;")) != std::string::npos; ) v.replace(p, 6, "'");
    for (size_t p; (p = v.find("&quot;")) != std::string::npos; ) v.replace(p, 6, "\"");
    for (size_t p; (p = v.find("&lt;")) != std::string::npos; ) v.replace(p, 4, "<");
    for (size_t p; (p = v.find("&gt;")) != std::string::npos; ) v.replace(p, 4, ">");
    for (size_t p; (p = v.find("&amp;")) != std::string::npos; ) v.replace(p, 5, "&");
    return v;
}

static std::string XmlAttr(const std::string& block, const char* attr) {
    std::string pat = std::string(attr) + "=\"";
    size_t a = block.find(pat);
    if (a == std::string::npos) return "";
    a += pat.size();
    size_t b = block.find('"', a);
    if (b == std::string::npos) return "";
    return block.substr(a, b - a);
}

static std::string AsciiFold(const std::string& in);   // defined below

// One batch of set names, answered by the user's own MAME. `out` gets one verdict per name, in order, or
// is left empty when MAME could not be asked at all (it would not start, or said nothing) - which the
// caller reads as "keep these, and remember nothing", so a broken MAME cannot empty somebody's library.
//
// A scan hands these over in batches because the names go on a command line, which Windows ends at
// about 32,000 characters; a few thousand sets in one call used to fail outright.
static void MameVerifyBatch(const std::wstring& mameExe, const std::vector<std::string>& names,
                            std::vector<phantom::MameVerdict>& out) {
    out.clear();
    if (mameExe.empty() || names.empty()) return;

    std::wstring dir;
    size_t slash = mameExe.find_last_of(L"\\/");
    if (slash != std::wstring::npos) dir = mameExe.substr(0, slash);

    std::wstring args;
    for (const auto& s : names) args += L" " + StringToWstring(s);

    // 1. bad romsets
    std::set<std::string> bad;
    std::string verify = RunCapture(L"\"" + mameExe + L"\" -verifyroms" + args, dir);
    {
        std::stringstream vs(verify);
        std::string line;
        while (std::getline(vs, line)) {
            // "romset <name> is bad"
            if (line.rfind("romset ", 0) != 0) continue;
            size_t sp = line.find(' ', 7);
            if (sp == std::string::npos) continue;
            if (line.find(" is bad") != std::string::npos) bad.insert(line.substr(7, sp - 7));
        }
    }

    // 2. devices and other non-runnable machines, plus everything worth showing on the cabinet. Each
    //    machine's block is bounded at </machine> before anything is read out of it: a driver's block
    //    is followed by blocks for every device it references, which carry <description> tags of their own.
    std::string xml = RunCapture(L"\"" + mameExe + L"\" -listxml" + args, dir);
    if (verify.empty() && xml.size() <= 64) return;   // MAME did not answer

    std::set<std::string> notRunnable;
    std::map<std::string, MameMeta> meta;
    size_t at = 0;
    while ((at = xml.find("<machine name=\"", at)) != std::string::npos) {
        size_t q = at + 15;
        size_t e = xml.find('"', q);
        if (e == std::string::npos) break;
        std::string name = xml.substr(q, e - q);

        size_t tagEnd = xml.find('>', e);
        if (tagEnd == std::string::npos) break;
        std::string openTag = xml.substr(e, tagEnd - e);
        if (openTag.find("runnable=\"no\"") != std::string::npos) notRunnable.insert(name);

        size_t blockEnd = xml.find("</machine>", tagEnd);
        if (blockEnd == std::string::npos) blockEnd = xml.size();
        std::string block = xml.substr(tagEnd, blockEnd - tagEnd);

        MameMeta m;
        m.year = XmlTag(block, "year");
        m.manufacturer = XmlTag(block, "manufacturer");

        size_t disp = block.find("<display ");
        if (disp != std::string::npos) {
            size_t dEnd = block.find('>', disp);
            std::string d = block.substr(disp, (dEnd == std::string::npos ? block.size() : dEnd) - disp);
            try { m.width   = std::stoi(XmlAttr(d, "width")); }   catch (...) {}
            try { m.height  = std::stoi(XmlAttr(d, "height")); }  catch (...) {}
            try { m.rotate  = std::stoi(XmlAttr(d, "rotate")); }  catch (...) {}
            try { m.refresh = std::stod(XmlAttr(d, "refresh")); } catch (...) {}
        }
        if (!m.year.empty() || !m.manufacturer.empty() || m.width) meta[name] = m;

        at = blockEnd;
    }

    for (const auto& n : names) {
        phantom::MameVerdict v;
        v.runnable = !bad.count(n) && !notRunnable.count(n);
        auto mi = meta.find(n);
        if (mi != meta.end()) {
            v.year = AsciiFold(mi->second.year);
            v.maker = AsciiFold(mi->second.manufacturer);
            if (mi->second.width && mi->second.height) {
                char buf[96];
                const bool tate = (mi->second.rotate == 90 || mi->second.rotate == 270);
                if (mi->second.refresh > 0.0)
                    snprintf(buf, sizeof(buf), "%dx%d @ %.2fHz%s", mi->second.width, mi->second.height, mi->second.refresh, tate ? " TATE" : "");
                else
                    snprintf(buf, sizeof(buf), "%dx%d%s", mi->second.width, mi->second.height, tate ? " TATE" : "");
                v.mode = buf;
            }
        }
        out.push_back(std::move(v));
    }
}

// ---- PS3 titles --------------------------------------------------------------------
//
// An installed PS3 game is a directory named after its title id - NPUA80105 - which is
// no use on a cabinet. The real name sits in PARAM.SFO beside it, so read that.
//
// SFO is a small, fixed binary format: a header giving the offsets of a key table and a
// data table, then one 16-byte index entry per field. No library needed.
static std::string ReadSfoTitle(const fs::path& sfo) {
    std::ifstream f(sfo.string().c_str(), std::ios::binary);
    if (!f.is_open()) return "";
    std::string buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (buf.size() < 20) return "";

    const unsigned char* d = (const unsigned char*)buf.data();
    auto u32 = [&](size_t o) -> uint32_t {
        if (o + 4 > buf.size()) return 0;
        return (uint32_t)d[o] | ((uint32_t)d[o + 1] << 8) |
               ((uint32_t)d[o + 2] << 16) | ((uint32_t)d[o + 3] << 24);
    };
    auto u16 = [&](size_t o) -> uint16_t {
        if (o + 2 > buf.size()) return 0;
        return (uint16_t)(d[o] | (d[o + 1] << 8));
    };

    if (u32(0) != 0x46535000u) return "";          // "\0PSF"
    uint32_t keyTable  = u32(8);
    uint32_t dataTable = u32(12);
    uint32_t entries   = u32(16);
    if (entries == 0 || entries > 1024) return "";

    std::string best;
    for (uint32_t i = 0; i < entries; i++) {
        size_t e = 20 + (size_t)i * 16;
        if (e + 16 > buf.size()) break;
        uint32_t keyOff  = u16(e);
        uint32_t dataLen = u32(e + 4);
        uint32_t dataOff = u32(e + 12);

        size_t k = keyTable + keyOff;
        if (k >= buf.size()) continue;
        std::string key;
        while (k < buf.size() && d[k]) key.push_back((char)d[k++]);

        // TITLE is the plain name; TITLE_xx are localisations, which we ignore so the
        // result does not depend on which happens to come first.
        if (key != "TITLE") continue;

        size_t v = dataTable + dataOff;
        if (v >= buf.size()) continue;
        size_t n = dataLen;
        if (v + n > buf.size()) n = buf.size() - v;
        std::string val((const char*)d + v, n);
        while (!val.empty() && (val.back() == '\0' || val.back() == ' ')) val.pop_back();
        if (!val.empty()) { best = val; break; }
    }
    return best;
}


// ---- Xbox: the title lives in the disc's XBE, not in its filename --------------------
//
// A scene release is called "eps-h2u.iso" and a redump is called something nearly as
// unhelpful, so the only trustworthy name for an Xbox game is the one the executable
// carries. default.xbe holds a certificate with the title, the publisher and the build
// date, which is what the cabinet should be showing.
//
// Two formats have to be read to get there: XDVDFS, the disc filesystem, to find
// default.xbe; and the XBE header, to find the certificate inside it. Both are simple and
// fixed, and only a few KB is ever read - the ISO is not scanned.

struct XboxMeta {
    std::string title;
    std::string year;
    std::string maker;
};

static const unsigned XISO_SECTOR = 2048;

static bool ReadAt(std::ifstream& f, unsigned long long off, void* dst, size_t len) {
    f.clear();
    f.seekg((std::streamoff)off, std::ios::beg);
    if (!f) return false;
    f.read((char*)dst, (std::streamsize)len);
    return (size_t)f.gcount() == len;
}

static uint32_t Rd32(const unsigned char* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t Rd16(const unsigned char* p) {
    return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}

// Walk the directory tree for a name. Entries are a binary tree whose child links are
// 4-byte-unit offsets into the same table; `seen` stops a malformed image looping.
static bool XisoFind(const std::vector<unsigned char>& table, const std::string& want,
                     uint32_t& outSector, uint32_t& outSize) {
    std::vector<uint32_t> stack;
    std::set<uint32_t> seen;
    stack.push_back(0);
    while (!stack.empty()) {
        const uint32_t off = stack.back();
        stack.pop_back();
        if (!seen.insert(off).second) continue;
        const size_t p = (size_t)off * 4;
        if (p + 14 > table.size()) continue;

        const uint16_t left = Rd16(&table[p]);
        const uint16_t right = Rd16(&table[p + 2]);
        const uint32_t sector = Rd32(&table[p + 4]);
        const uint32_t size = Rd32(&table[p + 8]);
        const unsigned char nlen = table[p + 13];
        if (p + 14 + nlen > table.size()) continue;

        std::string name((const char*)&table[p + 14], nlen);
        if (_stricmp(name.c_str(), want.c_str()) == 0) {
            outSector = sector;
            outSize = size;
            return true;
        }
        if (left) stack.push_back(left);
        if (right) stack.push_back(right);
    }
    return false;
}

static XboxMeta XboxMetaFor(const fs::path& iso) {
    XboxMeta meta;

    std::ifstream f(iso, std::ios::binary);
    if (!f) return meta;

    // The game partition starts at 0 in a plain xiso; a full dump carries a video
    // partition in front of it. These are the starts seen in the wild.
    static const unsigned long long BASES[] = { 0ull, 0x18300000ull, 0x02080000ull, 0x0FD90000ull };
    static const char MAGIC[20] = { 'M','I','C','R','O','S','O','F','T','*','X','B','O','X','*','M','E','D','I','A' };

    unsigned char vd[XISO_SECTOR];
    unsigned long long base = 0;
    bool haveBase = false;
    for (unsigned long long b : BASES) {
        if (!ReadAt(f, b + 32ull * XISO_SECTOR, vd, sizeof(vd))) continue;
        if (memcmp(vd, MAGIC, sizeof(MAGIC)) == 0) { base = b; haveBase = true; break; }
    }
    if (!haveBase) return meta;

    const uint32_t rootSector = Rd32(&vd[20]);
    const uint32_t rootSize = Rd32(&vd[24]);
    // A root table is a few KB; anything larger is a malformed or hostile image.
    if (rootSize == 0 || rootSize > 1u * 1024u * 1024u) return meta;

    std::vector<unsigned char> table(rootSize);
    if (!ReadAt(f, base + (unsigned long long)rootSector * XISO_SECTOR, table.data(), rootSize))
        return meta;

    uint32_t xbeSector = 0, xbeSize = 0;
    if (!XisoFind(table, "default.xbe", xbeSector, xbeSize)) return meta;
    if (xbeSize < 0x200) return meta;

    unsigned char hdr[0x200];
    if (!ReadAt(f, base + (unsigned long long)xbeSector * XISO_SECTOR, hdr, sizeof(hdr)))
        return meta;
    if (memcmp(hdr, "XBEH", 4) != 0) return meta;

    // The certificate is given as a virtual address; the XBE's own base address turns it
    // back into a file offset.
    const uint32_t baseAddr = Rd32(&hdr[0x104]);
    const uint32_t certAddr = Rd32(&hdr[0x118]);
    if (certAddr < baseAddr) return meta;
    const uint32_t certOff = certAddr - baseAddr;
    if (certOff + 0xA4 > xbeSize) return meta;

    unsigned char cert[0xA4];
    if (!ReadAt(f, base + (unsigned long long)xbeSector * XISO_SECTOR + certOff, cert, sizeof(cert)))
        return meta;

    // Title name: 40 UTF-16LE characters, NUL padded.
    std::wstring wtitle;
    for (int i = 0; i < 40; i++) {
        const wchar_t c = (wchar_t)Rd16(&cert[0x0C + i * 2]);
        if (!c) break;
        wtitle.push_back(c);
    }
    while (!wtitle.empty() && (wtitle.back() == L' ' || wtitle.back() == L'\t')) wtitle.pop_back();
    if (!wtitle.empty()) meta.title = WstringToString(wtitle);

    // The build date. Not the release date, but the right year, which is all the cabinet
    // shows - and a wrong one is worse than none, so an implausible value is dropped.
    const uint32_t when = Rd32(&cert[0x04]);
    if (when > 883612800u && when < 1577836800u) {   // 1998-01-01 .. 2020-01-01
        const time_t t = (time_t)when;
        struct tm tmv;
        if (gmtime_s(&tmv, &t) == 0) {
            char buf[8];
            snprintf(buf, sizeof(buf), "%d", tmv.tm_year + 1900);
            meta.year = buf;
        }
    }

    // The publisher is the top half of the title id, as two ASCII letters.
    const uint32_t titleId = Rd32(&cert[0x08]);
    const char pub[3] = { (char)((titleId >> 24) & 0xFF), (char)((titleId >> 16) & 0xFF), 0 };
    if (isalnum((unsigned char)pub[0]) && isalnum((unsigned char)pub[1])) {
        static const struct { const char* code; const char* name; } PUBS[] = {
            { "MS", "Microsoft" },       { "AC", "Acclaim" },        { "AQ", "Aqua System" },
            { "AT", "Atlus" },           { "AV", "Activision" },     { "BA", "Bandai" },
            { "BL", "Black Box" },       { "BM", "BAM! Entertainment" }, { "BS", "Bethesda" },
            { "BV", "Buena Vista" },     { "BW", "BBC Multimedia" }, { "CC", "Codemasters" },
            { "CK", "Capcom" },          { "CM", "Capcom" },         { "CV", "Crave" },
            { "DC", "DreamCatcher" },    { "EA", "Electronic Arts" },{ "EM", "Empire" },
            { "ES", "Eidos" },           { "FS", "From Software" },  { "GV", "Groove Games" },
            { "HW", "Highwaystar" },     { "IF", "Idea Factory" },   { "IG", "Infogrames" },
            { "JW", "JoWood" },          { "KN", "Konami" },         { "KO", "KOEI" },
            { "LA", "LucasArts" },       { "LS", "Black Bean" },     { "MD", "Midway" },
            { "ME", "Medix" },           { "MI", "Microids" },       { "MJ", "Majesco" },
            { "MM", "Myelin Media" },    { "MP", "MipMap" },         { "MW", "Midway" },
            { "MX", "Empire" },          { "NK", "NewKidCo" },       { "NL", "NovaLogic" },
            { "NM", "Namco" },           { "OX", "Oxygen" },         { "PC", "Playlogic" },
            { "PL", "Phantagram" },      { "RA", "Rage" },           { "SA", "Sammy" },
            { "SC", "SCi" },             { "SE", "SEGA" },           { "SN", "SNK" },
            { "SQ", "Square Enix" },     { "SS", "Simon & Schuster" },{ "SU", "Success" },
            { "SW", "Swing!" },          { "TA", "Takara" },         { "TC", "Tecmo" },
            { "TK", "Takuyo" },          { "TM", "TDK Mediactive" }, { "TQ", "THQ" },
            { "TS", "Titus" },           { "TT", "Take-Two" },       { "UA", "Unknown" },
            { "UB", "Ubisoft" },         { "VC", "Victor" },         { "VN", "Vivendi" },
            { "VU", "Vivendi" },         { "VV", "Vivendi" },        { "WE", "Wanadoo" },
            { "WR", "Warner Bros" },     { "XI", "XPEC" },           { "XK", "Xbox kiosk" },
            { "XL", "Xbox live" },       { "XM", "Evolved Games" },  { "XP", "XPEC" },
            { "XR", "Panorama" },        { "YB", "YBARRA" },         { "ZD", "Zushi" },
        };
        meta.maker = pub;
        for (const auto& e : PUBS) {
            if (_stricmp(e.code, pub) == 0) { meta.maker = e.name; break; }
        }
    }

    return meta;
}

// Find PARAM.SFO for a game file. An installed title keeps it beside USRDIR; a disc
// layout keeps it in PS3_GAME. Walk up a few levels rather than assuming either.
static std::string Ps3TitleFor(const fs::path& romFile) {
    fs::path p = romFile.parent_path();
    for (int up = 0; up < 4 && !p.empty(); up++) {
        std::error_code ec;
        fs::path cand = p / "PARAM.SFO";
        if (fs::exists(cand, ec)) {
            std::string t = ReadSfoTitle(cand);
            if (!t.empty()) return t;
        }
        p = p.parent_path();
    }
    return "";
}

// Fold a UTF-8 title down to ASCII for the cabinet.
//
// The launcher's font is ASCII 32..126 (phantom_font.h), so anything else draws as '?'.
// PARAM.SFO titles routinely carry symbols - "WipEout(R) HD" - and MAME descriptions
// carry accents, so this maps the common Latin-1 letters to their base form, drops
// decorative symbols, and collapses whatever that leaves.
static std::string JsonEscape(const std::string& in);   // defined below

static std::string AsciiFold(const std::string& in) {
    static const struct { unsigned cp; const char* rep; } MAP[] = {
        { 0x00AE, "" }, { 0x2122, "" }, { 0x00A9, "" },           // (R) (TM) (C)
        { 0x2013, "-" }, { 0x2014, "-" },                          // en/em dash
        { 0x2018, "'" }, { 0x2019, "'" },
        { 0x201C, "\"" }, { 0x201D, "\"" },
        { 0x2026, "..." }, { 0x00B7, "-" }, { 0x00D7, "x" },
        { 0x00E0, "a" }, { 0x00E1, "a" }, { 0x00E2, "a" }, { 0x00E4, "a" }, { 0x00E5, "a" },
        { 0x00E7, "c" }, { 0x00E8, "e" }, { 0x00E9, "e" }, { 0x00EA, "e" }, { 0x00EB, "e" },
        { 0x00EC, "i" }, { 0x00ED, "i" }, { 0x00EE, "i" }, { 0x00EF, "i" },
        { 0x00F1, "n" }, { 0x00F2, "o" }, { 0x00F3, "o" }, { 0x00F4, "o" }, { 0x00F6, "o" },
        { 0x00F9, "u" }, { 0x00FA, "u" }, { 0x00FB, "u" }, { 0x00FC, "u" },
        { 0x00C0, "A" }, { 0x00C1, "A" }, { 0x00C4, "A" }, { 0x00C7, "C" },
        { 0x00C8, "E" }, { 0x00C9, "E" }, { 0x00D6, "O" }, { 0x00DC, "U" }, { 0x00DF, "ss" },
    };

    std::string out;
    size_t i = 0;
    while (i < in.size()) {
        unsigned char c = (unsigned char)in[i];
        if (c < 0x80) { out.push_back((char)c); i++; continue; }

        // decode one UTF-8 sequence
        unsigned cp = 0;
        int len = 1;
        if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
        else { i++; continue; }                       // stray continuation byte
        if (i + len > in.size()) break;
        for (int k = 1; k < len; k++) cp = (cp << 6) | ((unsigned char)in[i + k] & 0x3F);
        i += len;

        const char* rep = NULL;
        for (const auto& m : MAP) if (m.cp == cp) { rep = m.rep; break; }
        if (rep) out += rep;                          // unknown codepoints are dropped
    }

    // tidy up whatever the drops left behind
    std::string tidy;
    for (size_t n = 0; n < out.size(); n++) {
        if (out[n] == ' ' && !tidy.empty() && tidy.back() == ' ') continue;
        tidy.push_back(out[n]);
    }
    while (!tidy.empty() && tidy.back() == ' ') tidy.pop_back();
    return tidy;
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

// ---- the library -----------------------------------------------------------------------------
//
// What the cabinet browses, and what a launch resolves against: one immutable phantom::Index, replaced
// whole when a scan publishes a new one. game_index.h says why it is shaped the way it is, and
// library_scan.h how it is built; this is the part that ties them to the manager's settings and window.
//
// The scan runs on its own thread. The window used to freeze for the whole of it, and the cabinet could
// not be answered until it ended; now the last scan is served from the moment the manager starts, a
// rescan runs behind it, and what it finds is published as it goes.
static std::mutex g_libMutex;
static std::shared_ptr<const phantom::Index> g_index;
static uint32_t g_lastVer = 0;
static std::atomic<bool> g_scanning(false);
static std::atomic<bool> g_scanQueued(false);
static std::atomic<bool> g_scanQueuedFull(false);
static std::atomic<bool> g_scanStop(false);
#define WM_APP_LIBRARY (WM_APP + 1)

static std::shared_ptr<const phantom::Index> CurrentIndex() {
    std::lock_guard<std::mutex> lk(g_libMutex);
    return g_index;
}

static std::wstring IndexCachePath() { return GetAppDir() + L"\\phantom_index.bin"; }

// Make `ix` what the cabinet sees. A scan that found nothing new changes nothing: publishing bumps the
// version, and a cabinet in the middle of browsing would throw away every page it had fetched for a
// library that had not changed.
static bool PublishIndex(std::shared_ptr<phantom::Index> ix) {
    const std::shared_ptr<const phantom::Index> cur = CurrentIndex();
    if (cur && phantom::IndexEquivalent(*cur, *ix)) return false;
    {
        std::lock_guard<std::mutex> lk(g_libMutex);
        uint32_t v = (uint32_t)time(NULL);   // a restart must not reuse a version a cabinet is still holding
        if (v <= g_lastVer) v = g_lastVer + 1;
        g_lastVer = v;
        ix->ver = v;
        g_index = ix;
    }
    if (hMainWnd) PostMessage(hMainWnd, WM_APP_LIBRARY, 0, 0);
    return true;
}

// The last scan, so there is a library to serve before the first new scan has finished. It is the answer
// only for the folders it was made from: a cache from before a folder was changed is not used.
static void LoadCachedLibrary() {
    std::shared_ptr<phantom::Index> ix = phantom::LoadIndex(IndexCachePath(), 0);
    if (!ix) return;
    for (size_t s = 0; s < ix->sys.size(); s++) {
        if (ix->lists[1 + s].empty()) continue;
        EmulatorDef* e = EmuByKey(ix->sys[s].key);
        if (!e || phantom::scan_detail::Utf8(GetText(e->romsEdit)) != ix->sys[s].root) return;
    }
    PublishIndex(ix);
}

// FBNeo is asked once which games it supports - a folder of MAME sets holds plenty it cannot start - and
// the answer is kept until the executable changes: asking costs a second or more and the list is ten
// megabytes of XML. Empty if it cannot be asked, which is read as "do not filter".
static std::map<std::string, FbneoSet> LoadFbneoSets(const std::wstring& fbExe) {
    std::map<std::string, FbneoSet> sets;
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (fbExe.empty() || !GetFileAttributesExW(fbExe.c_str(), GetFileExInfoStandard, &a)) return sets;

    char sig[128];
    snprintf(sig, sizeof(sig), "FBN1\t%lu\t%lu\t%lu", (unsigned long)a.nFileSizeLow,
             (unsigned long)a.ftLastWriteTime.dwHighDateTime, (unsigned long)a.ftLastWriteTime.dwLowDateTime);
    const std::wstring cache = GetAppDir() + L"\\fbneo_sets.cache";

    {
        std::ifstream in(cache.c_str(), std::ios::binary);
        std::string line;
        if (in && std::getline(in, line) && line == sig) {
            while (std::getline(in, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                std::string f[5];
                size_t at = 0;
                int n = 0;
                while (n < 5) {
                    const size_t tab = line.find('\t', at);
                    f[n++] = line.substr(at, tab == std::string::npos ? std::string::npos : tab - at);
                    if (tab == std::string::npos) break;
                    at = tab + 1;
                }
                if (n < 5 || f[0].empty()) continue;
                FbneoSet s;
                s.isBios = (f[1] == "1");
                s.description = f[2];
                s.year = f[3];
                s.manufacturer = f[4];
                sets[f[0]] = s;
            }
            if (sets.size() >= 1000) return sets;
            sets.clear();
        }
    }

    std::wstring fbDir;
    size_t slash = fbExe.find_last_of(L"\\/");
    if (slash != std::wstring::npos) fbDir = fbExe.substr(0, slash);
    sets = ParseFbneoDat(RunCapture(L"\"" + fbExe + L"\" -listinfo", fbDir));
    if (sets.size() < 1000) { sets.clear(); return sets; }

    std::ofstream out(cache.c_str(), std::ios::binary | std::ios::trunc);
    if (out) {
        auto clean = [](std::string s) { for (char& c : s) if (c == '\t' || c == '\n' || c == '\r') c = ' '; return s; };
        out << sig << "\n";
        for (const auto& kv : sets)
            out << clean(kv.first) << '\t' << (kv.second.isBios ? "1" : "0") << '\t' << clean(kv.second.description) << '\t'
                << clean(kv.second.year) << '\t' << clean(kv.second.manufacturer) << "\n";
    }
    return sets;
}

static std::string ExeSignature(const std::wstring& exe) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (exe.empty() || !GetFileAttributesExW(exe.c_str(), GetFileExInfoStandard, &a)) return "";
    char buf[96];
    snprintf(buf, sizeof(buf), "%lu.%lu.%lu", (unsigned long)a.nFileSizeLow, (unsigned long)a.ftLastWriteTime.dwHighDateTime,
             (unsigned long)a.ftLastWriteTime.dwLowDateTime);
    return buf;
}

static std::vector<phantom::ScanTarget> BuildScanTargets() {
    std::vector<phantom::ScanTarget> targets;
    for (int i = 0; i < g_emuCount; i++) {
        EmulatorDef& e = g_emus[i];
        phantom::ScanTarget t;
        t.key = e.key;
        t.name = e.sysName;
        t.videoMode = e.videoMode;
        t.exts = e.exts;
        t.roots.push_back(GetText(e.romsEdit));

        // RPCS3 keeps installed titles in its own dev_hdd0, not in a ROMs folder - a .pkg is an installer, so
        // that is where a PS3 game ends up once it has been added. Scanned as well as the ROMs path so an
        // installed game is found without anybody pointing a second setting at the emulator's own directory.
        if (!strcmp(e.key, "rpcs3")) {
            std::wstring exe = GetText(e.exeEdit);
            size_t slash = exe.find_last_of(L"\\/");
            if (slash != std::wstring::npos) {
                std::wstring hdd = exe.substr(0, slash) + L"\\dev_hdd0\\game";
                if (fs::exists(hdd)) t.roots.push_back(hdd);
            }
        }
        targets.push_back(std::move(t));
    }
    return targets;
}

static void RunLibraryScan(bool full) {
    std::vector<phantom::ScanTarget> targets = BuildScanTargets();

    // MAME's own names, so arcade entries list "Battle Garegga (Korea)" rather than "bgaregga". Empty if MAME
    // is not configured, in which case titles fall back to the ROM stem.
    SetWindowText(hStaticStatus, L"Status: Reading MAME's game list...");
    EmulatorDef* mameDef = EmuByKey("groovymame");
    const std::wstring mameExe = mameDef ? GetText(mameDef->exeEdit) : L"";
    const std::map<std::string, std::string> mameTitles = LoadMameTitles(mameExe);

    EmulatorDef* fbDef = EmuByKey("fbneo");
    const std::wstring fbExe = fbDef ? GetText(fbDef->exeEdit) : L"";
    SetWindowText(hStaticStatus, L"Status: Asking FBNeo which games it supports...");
    const std::map<std::string, FbneoSet> fbneoSets = LoadFbneoSets(fbExe);

    phantom::ScanServices svc;
    svc.mameTitles = &mameTitles;
    svc.fbneoSets = &fbneoSets;
    svc.ps3Title = [](const std::wstring& p) { return Ps3TitleFor(fs::path(p)); };
    svc.xboxMeta = [](const std::wstring& p) {
        XboxMeta m = XboxMetaFor(fs::path(p));
        phantom::Derived d;
        d.title = m.title;
        d.year = m.year;
        d.maker = m.maker;
        return d;
    };
    svc.fold = [](const std::string& s) { return AsciiFold(s); };
    if (!mameExe.empty() && fs::exists(mameExe)) {
        svc.mameVerify = [mameExe](const std::vector<std::string>& names, std::vector<phantom::MameVerdict>& out) {
            MameVerifyBatch(mameExe, names, out);
        };
        svc.mameFingerprint = ExeSignature(mameExe);
    }

    phantom::ScanOptions opt;
    opt.full = full;
    opt.threads = (int)std::min(8u, std::max(2u, std::thread::hardware_concurrency()));
    opt.rejectsFile = GetAppDir() + L"\\mame_rejects.txt";
    opt.stop = &g_scanStop;

    DWORD lastStatus = 0;
    auto progress = [&](const std::string& s) {
        const DWORD now = GetTickCount();
        if (now - lastStatus < 120) return;   // the window repaints for every message; a scan can send hundreds
        lastStatus = now;
        SetWindowText(hStaticStatus, (L"Status: " + StringToWstring(s)).c_str());
    };
    auto publish = [&](std::shared_ptr<phantom::Index> ix, bool final) {
        const std::shared_ptr<phantom::Index> keep = ix;
        const bool changed = PublishIndex(ix);
        if (final && changed) phantom::SaveIndex(*keep, IndexCachePath());
    };

    const phantom::ScanStats st = phantom::RunScan(targets, svc, opt, CurrentIndex(), publish, progress);

    char took[32];
    snprintf(took, sizeof(took), "%.1f", st.totalMs / 1000.0);
    std::wstring status = L"Status: " + std::to_wstring(st.listed) + L" game(s) listed in " + StringToWstring(took) + L" s.";
    if (st.mameRejected + st.mameRemembered > 0)
        status += L" (" + std::to_wstring(st.mameRejected + st.mameRemembered) + L" set(s) left out: MAME cannot run them.)";
    if (st.fbneoSkipped > 0) status += L" (" + std::to_wstring(st.fbneoSkipped) + L" file(s) left out: FBNeo does not support them.)";
    if (st.pkgs > 0) status += L" (" + std::to_wstring(st.pkgs) + L" PS3 .pkg installer(s) ignored - install them in RPCS3 first.)";
    SetWindowText(hStaticStatus, status.c_str());
}

static DWORD WINAPI LibraryScanThread(LPVOID param) {
    try { RunLibraryScan(param != NULL); } catch (...) {
        SetWindowText(hStaticStatus, L"Status: [FAILED] the library scan stopped with an error.");
    }
    g_scanning = false;
    if (g_scanQueued.exchange(false)) StartLibraryScan(g_scanQueuedFull.exchange(false));
    return 0;
}

// Scan the ROM folders in the background. `full` forgets everything learned before (what each file turned
// out to be, what MAME rejected) and asks again; otherwise only files that are new or changed are looked at.
// A request made while a scan is running is not lost: it runs when that one ends.
void StartLibraryScan(bool full) {
    if (g_scanning.exchange(true)) {
        if (full) g_scanQueuedFull = true;
        g_scanQueued = true;
        return;
    }
    HANDLE h = CreateThread(NULL, 0, LibraryScanThread, (LPVOID)(uintptr_t)(full ? 1 : 0), 0, NULL);
    if (h) CloseHandle(h); else g_scanning = false;
}

std::atomic<bool> g_launchInProgress(false);
// Set from the LAUNCH datagram: true when the core says it is sending a keyboard.
std::atomic<bool> g_misterKeyboard(false);

// Fill {rom}, {rom_stem}, {rom_dir} and {mister_ip} in an emulator's argument template.
static std::wstring ExpandArgs(const std::wstring& tmpl, const std::wstring& rom,
                               const std::wstring& stem, const std::wstring& misterIp) {
    // The folder the file sits in, with no trailing separator. 86Box is launched against a
    // VM directory rather than a file, and this is what turns its 86box.cfg back into one.
    std::wstring dir;
    const size_t slash = rom.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
        dir = rom.substr(0, slash);

    std::wstring out = tmpl;
    struct { const wchar_t* tag; const std::wstring& val; } subs[] = {
        { L"{rom_stem}",  stem },
        { L"{rom_dir}",   dir },
        { L"{rom}",       rom },
        { L"{mister_ip}", misterIp },
    };
    for (auto& s : subs) {
        size_t at;
        while ((at = out.find(s.tag)) != std::wstring::npos)
            out.replace(at, wcslen(s.tag), s.val);
    }
    return out;
}

// ---- telling the cabinet when a launch did not work ----------------------------------------------
//
// A launch that failed on the PC - the emulator would not start, stopped straight away, or sat there showing an error
// dialog - used to say so only in this window, while the cabinet, which had been told ACK:LAUNCH:OK, waited on "Host is
// starting the emulator..." for three minutes. The address the LAUNCH came from is remembered, and a failure goes back to
// it as LAUNCHFAIL:<one line the cabinet can show>.
static std::mutex g_launchClientMutex;
static sockaddr_in g_launchClient;
static bool g_launchClientValid = false;

static void NoteLaunchClient(const sockaddr_in& from) {
    std::lock_guard<std::mutex> lk(g_launchClientMutex);
    g_launchClient = from;
    g_launchClientValid = true;
}

static void ReportLaunchFailure(const std::string& reason) {
    sockaddr_in to;
    {
        std::lock_guard<std::mutex> lk(g_launchClientMutex);
        if (!g_launchClientValid) return;
        to = g_launchClient;
    }
    std::string msg = "LAUNCHFAIL:" + reason.substr(0, 88);
    for (char& c : msg) if ((unsigned char)c < 32 || (unsigned char)c > 126) c = ' ';   // the cabinet's font is ASCII
    SOCKET s = g_udpSocket;
    if (s != INVALID_SOCKET) sendto(s, msg.c_str(), (int)msg.size(), 0, (const sockaddr*)&to, sizeof(to));
}

static std::string AsciiLabel(const wchar_t* label) {
    return AsciiFold(phantom::scan_detail::Utf8(label ? std::wstring(label) : std::wstring()));
}

// RPCS3 will not run twice - the second one stops with "Another instance of RPCS3 is running" - and one left open at its
// game list, which is where it sits when somebody has been browsing it, blocks every launch from the cabinet. So the
// others are closed first: politely, then by force if one will not go.
static void CloseRunningInstances(const std::wstring& exePath) {
    const size_t sl = exePath.find_last_of(L"\\/");
    std::wstring name = sl == std::wstring::npos ? exePath : exePath.substr(sl + 1);
    for (auto& c : name) c = (wchar_t)towlower(c);
    std::vector<DWORD> pids = PidsOfProcessesNamed({ name }, /*needUdp=*/false);
    if (pids.empty()) return;

    struct Ctx { std::set<DWORD> pids; } ctx;
    ctx.pids.insert(pids.begin(), pids.end());
    EnumWindows([](HWND h, LPARAM l) -> BOOL {
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (((Ctx*)l)->pids.count(pid)) PostMessage(h, WM_CLOSE, 0, 0);
        return TRUE;
    }, (LPARAM)&ctx);

    for (int i = 0; i < 40; i++) {
        bool any = false;
        for (DWORD pid : pids) {
            HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
            if (h) { if (WaitForSingleObject(h, 0) == WAIT_TIMEOUT) any = true; CloseHandle(h); }
        }
        if (!any) return;
        Sleep(100);
    }
    for (DWORD pid : pids) {
        HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
        if (h) { TerminateProcess(h, 0); CloseHandle(h); }
    }
    Sleep(300);
}

// FBNeo only looks for a game's zip in its own ROM paths (szAppRomPaths in its ini), and out of the box those are the
// ROMs folder next to it. A library kept anywhere else - a shared MAME set on another drive, say - lists in the cabinet
// and then fails with "<game> was not found" the moment it is chosen. So the folder this app was pointed at is put in
// FBNeo's list: in the first empty slot, and only if it is not there already.
static void EnsureFbneoRomPath(const std::wstring& exeDir, const std::wstring& romsFolder) {
    if (exeDir.empty() || romsFolder.empty()) return;
    const fs::path ini = fs::path(exeDir) / L"config" / L"fcadefbneo.ini";
    std::error_code ec;
    if (!fs::exists(ini, ec)) return;   // FBNeo writes its own on first run; there is nothing to edit yet

    std::wstring want = romsFolder;
    for (auto& c : want) if (c == L'/') c = L'\\';
    if (want.back() != L'\\') want += L'\\';
    std::string wantA;
    for (wchar_t c : want) wantA.push_back(c < 128 ? (char)c : '?');
    if (wantA.find('?') != std::string::npos) return;   // not representable in its ANSI ini: leave it alone

    std::ifstream in(ini, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();

    auto lower = [](std::string s) { for (char& c : s) c = (char)tolower((unsigned char)c); return s; };
    const std::string wantLower = lower(wantA);
    size_t emptyLine = std::string::npos;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        std::string line = text.substr(pos, eol - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("szAppRomPaths[", 0) == 0) {
            const size_t close = line.find(']');
            const int slot = (close == std::string::npos) ? -1 : atoi(line.c_str() + 14);
            std::string value = close == std::string::npos ? "" : line.substr(close + 1);
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(value.begin());
            while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.pop_back();
            if (slot >= 0 && slot <= 5) {
                if (lower(value) == wantLower) return;                       // already there
                if (value.empty() && emptyLine == std::string::npos) emptyLine = pos;
            }
        }
        pos = eol + 1;
    }
    if (emptyLine == std::string::npos) return;   // all six are somebody's choice; do not overwrite one

    const size_t eol = text.find('\n', emptyLine);
    const size_t lineEnd = eol == std::string::npos ? text.size() : eol;
    std::string old = text.substr(emptyLine, lineEnd - emptyLine);
    const bool crlf = !old.empty() && old.back() == '\r';
    const size_t close = old.find(']');
    std::string repl = old.substr(0, close + 1) + " " + wantA + (crlf ? "\r" : "");
    text.replace(emptyLine, lineEnd - emptyLine, repl);

    const fs::path tmp = ini.wstring() + L".phantom.tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << text;
        if (!out) return;
    }
    fs::rename(tmp, ini, ec);
}

// A dialog box that is on screen and belongs to `pid`. FBNeo's "<game> was not found" is a standard dialog; RPCS3's
// "Another instance is running" is a Qt window that calls itself "Fatal Error". (Matching the word "Fatal" alone would
// catch "Fatal Fury", so it is "Fatal Error".)
static bool FindErrorWindow(DWORD pid, std::wstring& title) {
    struct Ctx { DWORD pid; std::wstring title; bool found; } c = { pid, L"", false };
    EnumWindows([](HWND h, LPARAM l) -> BOOL {
        Ctx* c = (Ctx*)l;
        DWORD p = 0;
        GetWindowThreadProcessId(h, &p);
        if (p != c->pid || !IsWindowVisible(h)) return TRUE;
        wchar_t cls[64] = {0}, ttl[256] = {0};
        GetClassNameW(h, cls, 64);
        GetWindowTextW(h, ttl, 256);
        const std::wstring t(ttl);
        const bool errorTitle = t.find(L"Fatal Error") != std::wstring::npos ||
                                (t.size() >= 5 && t.compare(t.size() - 5, 5, L"Error") == 0);
        if (!wcscmp(cls, L"#32770") || errorTitle) { c->found = true; c->title = t; return FALSE; }
        return TRUE;
    }, (LPARAM)&c);
    title = c.title;
    return c.found;
}

// Start the emulator a catalog id belongs to.
//
// The id is "<emulator key>_<rom stem>", which is how the cabinet's choice maps back to
// a row of g_emus without the launcher needing to know anything about the emulator.
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
    int port = GetPort();

    // split "<key>_<stem>"
    EmulatorDef* emu = NULL;
    std::string stem;
    for (int i = 0; i < g_emuCount; i++) {
        std::string prefix = std::string(g_emus[i].key) + "_";
        if (gameId.rfind(prefix, 0) == 0) {
            emu = &g_emus[i];
            stem = gameId.substr(prefix.size());
            break;
        }
    }
    if (!emu) {
        SetWindowText(hStaticStatus,
                      (L"Status: [FAILED] no emulator for id " + StringToWstring(gameId)).c_str());
        ReportLaunchFailure("No emulator is set up for that game.");
        return false;
    }
    const std::string label = AsciiLabel(emu->label);

    std::wstring exe  = GetText(emu->exeEdit);
    std::wstring roms = GetText(emu->romsEdit);
    if (exe.empty()) {
        SetWindowText(hStaticStatus,
                      (std::wstring(L"Status: [FAILED] no executable set for ") + emu->label).c_str());
        ReportLaunchFailure("No program is set for " + label + " in the PC app.");
        return false;
    }
    if (!fs::exists(exe)) {
        SetWindowText(hStaticStatus,
                      (std::wstring(L"Status: [FAILED] ") + emu->label + L" was not found at " + exe).c_str());
        ReportLaunchFailure(label + " was not found where the PC app expects it.");
        return false;
    }

    // Only GroovyMAME needs the address on the command line; the others are told once in
    // their own Settings > MiSTer page. Passing it to them would be an unknown option.
    if (misterIp.empty() && std::string(emu->key) == "groovymame") {
        SetWindowText(hStaticStatus,
                      L"Status: [FAILED] no MiSTer has been seen yet, so there is no address to stream to.");
        ReportLaunchFailure("The PC has not heard from a MiSTer yet.");
        return false;
    }

    // Use the path the scan actually recorded. Rebuilding it from the ROMs folder and the stem only works for
    // a file sitting directly in that folder, which stopped being true once the scan went recursive: an
    // installed PS3 game lives under RPCS3's own dev_hdd0, so the reconstruction produced C:\Games\PS3\NPUA80105
    // and RPCS3 refused it as an invalid folder. The index knows the real path, and finds it by id in
    // constant time - this used to read the whole catalog file and search it as text, on every launch.
    std::wstring wStem = StringToWstring(stem);
    std::wstring romPath;
    if (std::shared_ptr<const phantom::Index> ix = CurrentIndex()) {
        if (const phantom::Rec* r = ix->Find(gameId.c_str())) {
            romPath = StringToWstring(ix->Rom(*r));
            for (auto& c : romPath) if (c == L'/') c = L'\\';
        }
    }

    // Fall back to the old reconstruction for a catalog that predates this, trying each
    // extension the emulator declares.
    if (romPath.empty()) {
        std::wstring exts = emu->exts;
        size_t at = 0;
        while (at <= exts.size()) {
            size_t comma = exts.find(L',', at);
            std::wstring ext = exts.substr(at, (comma == std::wstring::npos ? exts.size() : comma) - at);
            if (!ext.empty()) {
                std::wstring cand = roms + L"\\" + wStem + ext;
                if (fs::exists(cand)) { romPath = cand; break; }
            }
            if (comma == std::wstring::npos) break;
            at = comma + 1;
        }
    }
    // GroovyMAME takes the set name, not a path, so a missing file is not fatal for it.
    if (romPath.empty()) romPath = roms + L"\\" + wStem;

    std::wstring args = ExpandArgs(emu->defArgs, romPath, wStem, misterIp);

    // Cabinet keyboard, only when the core told us it is sending one. MAME is the
    // only one of these that takes it on the command line; the rest have it in their
    // own MiSTer settings page alongside the address.
    if (g_misterKeyboard.load() && std::string(emu->key) == "groovymame" &&
        args.find(L"-keyboardprovider") == std::wstring::npos) {
        args += L" -keyboardprovider mister";
    }
    std::wstring cmd  = L"\"" + exe + L"\" " + args;

    std::wstring exeDir;
    size_t slashPos = exe.find_last_of(L"\\/");
    if (slashPos != std::wstring::npos) exeDir = exe.substr(0, slashPos);

    // What a particular emulator needs before it will start a game from here.
    if (std::string(emu->key) == "fbneo") EnsureFbneoRomPath(exeDir, roms);
    if (std::string(emu->key) == "rpcs3") CloseRunningInstances(exe);

    STARTUPINFO si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(0);

    BOOL ok = CreateProcessW(NULL, cmdBuf.data(), NULL, NULL, FALSE,
                             CREATE_NEW_CONSOLE | NORMAL_PRIORITY_CLASS, NULL,
                             exeDir.empty() ? NULL : exeDir.c_str(), &si, &pi);

    if (!ok) {
        DWORD err = GetLastError();
        std::wstring stat = std::wstring(L"Status: Could not start ") + emu->label +
                            L" (Error " + std::to_wstring(err) + L"). Check the executable path.";
        SetWindowText(hStaticStatus, stat.c_str());
        ReportLaunchFailure("Could not start " + label + " (Windows error " + std::to_string(err) + ").");
        return false;
    }

    g_activePid = pi.dwProcessId;
    {
        FILETIME c, e, k, u;
        g_activeStart = GetProcessTimes(pi.hProcess, &c, &e, &k, &u)
                            ? (((ULONGLONG)c.dwHighDateTime << 32) | c.dwLowDateTime) : 0;
    }
    CloseHandle(pi.hThread);

    // Spawning successfully is not the same as running. An emulator that cannot open what it was given either exits
    // within a second or two - a missing set, a file it does not support - or stays up behind a dialog box that
    // nobody is there to click. Both used to look, from the cabinet, like a stream that was merely slow. So the new
    // process is watched until it is plainly streaming (it owns a UDP socket, which is what a stream is), or has
    // stopped, or is showing an error; and if it is none of those after a while it is assumed to be a slow start
    // (RPCS3 compiling shaders on a first boot can take minutes). We are on the launch worker thread, so waiting
    // is free.
    const ULONGLONG t0 = GetTickCount64();
    while (GetTickCount64() - t0 < 14000) {
        if (WaitForSingleObject(pi.hProcess, 250) == WAIT_OBJECT_0) {
            DWORD code = 0;
            GetExitCodeProcess(pi.hProcess, &code);
            CloseHandle(pi.hProcess);
            g_activePid = 0;
            std::wstring stat = std::wstring(L"Status: [FAILED] ") + wStem + L" exited immediately (code " +
                                std::to_wstring(code) + L"). Missing ROM, or not a playable set?";
            SetWindowText(hStaticStatus, stat.c_str());
            ReportLaunchFailure(label + " stopped straight away (code " + std::to_string(code) + "). Is the game file usable?");
            return false;
        }
        std::wstring dlg;
        if (FindErrorWindow(pi.dwProcessId, dlg)) {
            CloseHandle(pi.hProcess);
            g_activePid = 0;   // it is not streaming and the cabinet should not be told a game is running
            SetWindowText(hStaticStatus, (std::wstring(L"Status: [FAILED] ") + emu->label + L" is showing \"" + dlg +
                                          L"\" - read it on the PC.").c_str());
            ReportLaunchFailure(label + " is showing an error window on the PC: " + AsciiFold(phantom::scan_detail::Utf8(dlg)));
            return false;
        }
        if (PidsOwningUdpEndpoint().count(pi.dwProcessId)) break;   // streaming
    }
    CloseHandle(pi.hProcess);

    std::wstring stat = std::wstring(L"Status: [RUNNING] ") + emu->label + L" (" + wStem +
                        L") -> streaming to MiSTer " + (misterIp.empty() ? L"(set in emulator)" : misterIp) +
                        L":" + std::to_wstring(port);
    SetWindowText(hStaticStatus, stat.c_str());
    return true;
}

struct LaunchTaskParams {
    std::string gameId;
    std::wstring misterIp;
};

static DWORD WINAPI DelayedLaunchWorker(LPVOID lpParam) {
    LaunchTaskParams* params = (LaunchTaskParams*)lpParam;
    std::string gid = params->gameId;
    std::wstring ip = params->misterIp;
    delete params;

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

    // No delay. It existed because the old flow switched the FPGA to Groovy.rbf at
    // launch time and the PC had to wait out the reconfiguration. The merged core never
    // switches - it is already loaded, already listening, and sits on the launcher until
    // video arrives - so there is nothing left to wait for.
    LaunchTaskParams* params = new LaunchTaskParams{ gameId, targetMisterIp };
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
                // Streamed in pieces and ended by closing the connection: a library of hundreds of thousands is
                // far too big to build as one string first.
                const std::string head = "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\n"
                                         "Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n";
                send(clientSock, head.c_str(), (int)head.length(), 0);

                std::shared_ptr<const phantom::Index> ix = CurrentIndex();
                std::string chunk = "{\"games\":[";
                auto flush = [&]() {
                    const char* p = chunk.data();
                    size_t n = chunk.size();
                    while (n) {
                        int k = send(clientSock, p, (int)std::min<size_t>(n, 1u << 20), 0);
                        if (k <= 0) { chunk.clear(); return false; }
                        p += k;
                        n -= (size_t)k;
                    }
                    chunk.clear();
                    return true;
                };
                bool ok = true;
                if (ix) {
                    for (size_t i = 0; i < ix->Total() && ok; i++) {
                        const phantom::Rec& r = ix->recs[ix->lists[0][i]];
                        const char* rom = ix->Rom(r);
                        const char* slash = strrchr(rom, '/');
                        if (i) chunk += ",";
                        chunk += "{\"id\":\"" + JsonEscape(ix->Id(r)) + "\",\"title\":\"" + JsonEscape(ix->Title(r)) +
                                 "\",\"system\":\"" + JsonEscape(ix->sys[r.sys].key) + "\",\"systemName\":\"" +
                                 JsonEscape(ix->sys[r.sys].name) + "\",\"romName\":\"" + JsonEscape(slash ? slash + 1 : rom) +
                                 "\",\"romPath\":\"" + JsonEscape(rom) + "\",\"videoMode\":\"" + JsonEscape(ix->Str(r.mode)) +
                                 "\",\"year\":\"" + JsonEscape(ix->Str(r.year)) + "\",\"manufacturer\":\"" +
                                 JsonEscape(ix->Str(r.maker)) + "\",\"resolution\":\"" + JsonEscape(ix->Str(r.mode)) + "\"}";
                        if (chunk.size() > (1u << 18)) ok = flush();
                    }
                }
                chunk += "]}";
                if (ok) flush();
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



// ---- is an emulator running? ----------------------------------------------------------
//
// Answers the cabinet's STATUS request; the checks themselves live in emulator_status.h so
// they can be tested against a real process (emulator_status_test.cpp).
//
// The emulator we started. g_activePid is only cleared on a KILL or a relaunch, never when
// the emulator simply exits, so it says nothing on its own: the process has to be looked at.
static bool LaunchedEmulatorAlive() {
    return ProcessAliveSince(g_activePid.load(), g_activeStart.load());
}

// Any emulator that is streaming, by the executables named in the settings. A Fightcade match
// is started by the Fightcade client, not by us, and that is precisely the case that must not
// be interrupted. An emulator that is only open has no UDP socket, so it does not count: with
// RPCS3 left at its game list, every other game's exit would otherwise look like a blip and
// the launcher would stay away for the cabinet's whole hold.
static std::set<std::wstring> ConfiguredEmulatorNames() {
    std::set<std::wstring> names;
    for (int i = 0; i < g_emuCount; i++) {
        std::wstring exe = GetText(g_emus[i].exeEdit);
        size_t sl = exe.find_last_of(L"\\/");
        if (sl != std::wstring::npos) exe = exe.substr(sl + 1);
        for (auto& c : exe) c = (wchar_t)towlower(c);
        if (!exe.empty()) names.insert(exe);
    }
    return names;
}

static bool AnyConfiguredEmulatorRunning() {
    return AnyProcessNamed(ConfiguredEmulatorNames(), /*needUdp=*/true);
}

static bool EmulatorRunning() {
    return LaunchedEmulatorAlive() || AnyConfiguredEmulatorRunning();
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
                // The older cabinets' way: the whole list in one datagram. They get a compact one, the small
                // systems first, so a long arcade list cannot crowd every other emulator off it.
                std::shared_ptr<const phantom::Index> ix = CurrentIndex();
                std::string catData = ix ? phantom::CompactCatalogJson(*ix) : "{\"games\":[]}";
                sendto(g_udpSocket, catData.c_str(), (int)std::min<size_t>(catData.length(), 60000), 0, (sockaddr*)&clientAddr, clientLen);
            } else if (msg == "CATINFO" || msg.rfind("LETTERS:", 0) == 0 || msg.rfind("LIST:", 0) == 0) {
                // The paged catalog. Each answer is one datagram of a few kilobytes at most, however big the library.
                std::string reply;
                if (phantom::AnswerCatalogRequest(CurrentIndex(), g_scanning.load(), msg, reply))
                    sendto(g_udpSocket, reply.c_str(), (int)reply.length(), 0, (sockaddr*)&clientAddr, clientLen);
            } else if (msg == "RESCAN") {
                // Y on the cabinet: look for new and changed games now. Only what changed is examined.
                StartLibraryScan(false);
                const char* reply = "RESCAN:OK";
                sendto(g_udpSocket, reply, (int)strlen(reply), 0, (sockaddr*)&clientAddr, clientLen);
            } else if (msg.rfind("LAUNCH:@", 0) == 0) {
                // A launch by position - LAUNCH:@<version>:<list>:<index>[:kbd=N] - which is how the paged cabinet
                // asks: it holds a window of the list, not every id. The version is what makes this safe: if the
                // library has been rescanned since the cabinet fetched the page, the position may now be a different
                // game, and the answer is "stale", not a launch of whatever is there.
                NoteLaunchClient(clientAddr);
                std::string spec = msg.substr(8);
                while (!spec.empty() && (spec.back() == '\r' || spec.back() == '\n' || spec.back() == ' ')) spec.pop_back();
                g_misterKeyboard.store(spec.find(":kbd=1") != std::string::npos);

                std::string reply;
                const std::shared_ptr<const phantom::Index> ix = CurrentIndex();
                uint32_t ver = 0, at = 0;
                std::vector<std::string> f;
                {
                    size_t pos = 0;
                    for (int n = 0; n < 4; n++) {
                        const size_t c = spec.find(':', pos);
                        f.push_back(spec.substr(pos, c == std::string::npos ? std::string::npos : c - pos));
                        if (c == std::string::npos) break;
                        pos = c + 1;
                    }
                }
                const bool parsed = f.size() >= 3 && phantom::detail::ToU32(f[0], ver) && phantom::detail::ToU32(f[2], at);
                if (!parsed) {
                    reply = "ACK:LAUNCH:BAD";
                } else if (!ix || ix->ver != ver) {
                    reply = "ACK:LAUNCH:STALE:" + std::to_string(ix ? ix->ver : 0);
                } else {
                    const phantom::Rec* r = ix->RecAt(ix->ListFor(f[1]), at);
                    if (!r) {
                        reply = "ACK:LAUNCH:NOTFOUND";
                    } else {
                        const std::string id = ix->Id(*r);
                        LaunchGame(id, misterClientIp);
                        reply = "ACK:LAUNCH:OK:" + id;
                    }
                }
                sendto(g_udpSocket, reply.c_str(), (int)reply.length(), 0, (sockaddr*)&clientAddr, clientLen);
            } else if (msg.rfind("LAUNCH:", 0) == 0) {
                // MiSTer requested emulator game launch!
                NoteLaunchClient(clientAddr);
                std::string gameId = msg.substr(7);
                while (!gameId.empty() && (gameId.back() == '\r' || gameId.back() == '\n' || gameId.back() == ' ')) {
                    gameId.pop_back();
                }

                // The core appends :kbd=1 when its Server > PS2 option is sending a
                // keyboard. Only then is it safe to point MAME at -keyboardprovider
                // mister: doing it unconditionally would take away the PC keyboard and
                // replace it with a keyboard the core is not transmitting, leaving no
                // input at all. Older cores send no suffix and simply get the default.
                g_misterKeyboard.store(gameId.find(":kbd=1") != std::string::npos);

                size_t sfx = gameId.find(':');
                if (sfx != std::string::npos) {
                    gameId = gameId.substr(0, sfx);
                }

                LaunchGame(gameId, misterClientIp);

                std::string reply = "ACK:LAUNCH:OK:" + gameId;
                sendto(g_udpSocket, reply.c_str(), (int)reply.length(), 0, (sockaddr*)&clientAddr, clientLen);
            } else if (msg == "KILL") {
                // "Stop the game": the one this daemon started, and also any configured emulator
                // that is streaming but was started by something else - a Fightcade match is started
                // by the Fightcade client, which leaves the daemon with no pid for it. Without this
                // the core closed the stream, the emulator's auto-reconnect reopened it a moment
                // later, and "return to launcher" only flickered. An emulator that is merely open
                // owns no UDP socket, so it is left alone.
                std::set<DWORD> toKill;
                if (g_activePid > 0) toKill.insert(g_activePid);
                for (DWORD pid : PidsOfProcessesNamed(ConfiguredEmulatorNames(), /*needUdp=*/true))
                    toKill.insert(pid);
                for (DWORD pid : toKill) {
                    HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
                    if (hProc) {
                        TerminateProcess(hProc, 0);
                        CloseHandle(hProc);
                    }
                }
                g_activePid = 0;
                const char* reply = "ACK:KILL:OK";
                sendto(g_udpSocket, reply, (int)strlen(reply), 0, (sockaddr*)&clientAddr, clientLen);
            } else if (msg == "PING") {
                const char* reply = "PONG";
                sendto(g_udpSocket, reply, (int)strlen(reply), 0, (sockaddr*)&clientAddr, clientLen);
            } else if (msg == "STATUS") {
                // Asked by the cabinet whenever a stream ends: see EmulatorRunning().
                const char* reply = EmulatorRunning() ? "STATUS:RUNNING" : "STATUS:IDLE";
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
    const std::wstring path(exePath);

    // Listing rules needs no elevation, so look first. This used to add a rule on every start, which put an
    // elevation prompt in front of the user each time and left a duplicate rule behind each time - dozens of
    // them. Done on its own thread: netsh takes a moment, and nothing waits on the answer.
    std::thread([path]() {
        std::string listing = RunCapture(L"netsh advfirewall firewall show rule name=\"Phantom Arcade Manager\" verbose", L"");
        std::string want = WstringToString(path);
        for (char& c : listing) c = (char)tolower((unsigned char)c);
        for (char& c : want) c = (char)tolower((unsigned char)c);
        if (!want.empty() && listing.find(want) != std::string::npos) return;   // this program is already allowed

        std::wstring cmd = L"advfirewall firewall add rule name=\"Phantom Arcade Manager\" dir=in action=allow program=\"" +
                           path + L"\" enable=yes profile=any";
        ShellExecute(NULL, L"runas", L"netsh", cmd.c_str(), NULL, SW_HIDE);
    }).detach();
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

// Launch whatever is highlighted in the manager's own list.
static void LaunchSelectedGame() {
    const int sel = (int)SendMessage(hListGames, LB_GETCURSEL, 0, 0);
    std::shared_ptr<const phantom::Index> ix = CurrentIndex();
    const phantom::Rec* r = (sel == LB_ERR || !ix) ? nullptr : ix->RecAt(0, (size_t)sel);
    if (!r) {
        SetWindowText(hStaticStatus, L"Status: pick a game from the list first.");
        return;
    }
    LaunchGame(ix->Id(*r));
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


        // One pair of rows per emulator, generated from g_emus.
        for (int i = 0; i < g_emuCount; i++) {
            EmulatorDef& e = g_emus[i];

            y += 32;
            std::wstring exeLabel = std::wstring(e.label) + L" Executable:";
            CreateWindow(L"STATIC", exeLabel.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, 20, y, 170, 20, hWnd, NULL, hInst, NULL);
            e.exeEdit = CreateWindow(L"EDIT", e.defExe, WS_CHILD | WS_VISIBLE | WS_BORDER,
                                     195, y, 350, 22, hWnd, (HMENU)(INT_PTR)IDC_EMU_EXE(i), hInst, NULL);
            CreateWindow(L"BUTTON", L"Browse...", WS_CHILD | WS_VISIBLE,
                         555, y, 85, 22, hWnd, (HMENU)(INT_PTR)IDC_EMU_EXE_BROWSE(i), hInst, NULL);

            y += 26;
            std::wstring romLabel = std::wstring(e.label) + L" ROMs Folder:";
            CreateWindow(L"STATIC", romLabel.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, 20, y, 170, 20, hWnd, NULL, hInst, NULL);
            e.romsEdit = CreateWindow(L"EDIT", e.defRoms, WS_CHILD | WS_VISIBLE | WS_BORDER,
                                      195, y, 350, 22, hWnd, (HMENU)(INT_PTR)IDC_EMU_ROMS(i), hInst, NULL);
            CreateWindow(L"BUTTON", L"Browse...", WS_CHILD | WS_VISIBLE,
                         555, y, 85, 22, hWnd, (HMENU)(INT_PTR)IDC_EMU_ROMS_BROWSE(i), hInst, NULL);
        }

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
        // No-data and owner-drawn: the list box keeps a count and asks for the text of whatever row is on screen,
        // so a library of hundreds of thousands costs it nothing. (A normal list box stores every string.)
        hListGames = CreateWindow(L"LISTBOX", NULL, WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | LBS_NOTIFY | LBS_NODATA | LBS_OWNERDRAWFIXED,
                                  20, y, 620, 120, hWnd, (HMENU)IDC_LIST_GAMES, hInst, NULL);

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
            LaunchSelectedGame();
            break;
        }

        // Browse buttons for every emulator row, resolved back to the table by id.
        // Ahead of the switch because the ids are a computed range, not constants.
        if (wmId >= IDC_EMU_BASE && wmId < IDC_EMU_BASE + g_emuCount * 4) {
            int idx = (wmId - IDC_EMU_BASE) / 4;
            int which = (wmId - IDC_EMU_BASE) % 4;
            EmulatorDef& e = g_emus[idx];
            if (which == 1) {
                // a Win32 filter needs embedded NULs, so it has to be a literal, not a concat
                auto path = BrowseFile(hWnd, L"Executable (*.exe)\0*.exe\0All Files (*.*)\0*.*\0");
                if (!path.empty()) { SetWindowText(e.exeEdit, path.c_str()); SaveConfiguration(); }
            } else if (which == 3) {
                std::wstring title = std::wstring(L"Select ") + e.label + L" ROMs Folder";
                auto path = BrowseFolder(hWnd, title.c_str());
                if (!path.empty()) { SetWindowText(e.romsEdit, path.c_str()); SaveConfiguration(); }
            }
            break;
        }

        switch (wmId) {
        case IDC_BTN_SAVE_CONFIG:
            SaveConfiguration();
            break;
        case IDC_BTN_SCAN_ROMS:
            // The button is the "look again from scratch" one; the cabinet's Y button is the quick one.
            StartLibraryScan(true);
            break;
        case IDC_BTN_LAUNCH_GAME:
            LaunchSelectedGame();
            break;
        case IDC_BTN_TOGGLE_DAEMON:
            ToggleDaemon();
            break;
        }
        break;
    }

    case WM_APP_LIBRARY: {
        std::shared_ptr<const phantom::Index> ix = CurrentIndex();
        SendMessage(hListGames, LB_SETCOUNT, (WPARAM)(ix ? ix->Total() : 0), 0);
        InvalidateRect(hListGames, NULL, TRUE);
        break;
    }

    case WM_MEASUREITEM: {
        MEASUREITEMSTRUCT* mi = (MEASUREITEMSTRUCT*)lParam;
        if (mi->CtlID == IDC_LIST_GAMES) { mi->itemHeight = 16; return TRUE; }
        return DefWindowProc(hWnd, msg, wParam, lParam);
    }

    case WM_DRAWITEM: {
        DRAWITEMSTRUCT* di = (DRAWITEMSTRUCT*)lParam;
        if (di->CtlID != IDC_LIST_GAMES) return DefWindowProc(hWnd, msg, wParam, lParam);
        if ((int)di->itemID < 0) return TRUE;   // an empty list asks to draw its focus rectangle

        const bool selected = (di->itemState & ODS_SELECTED) != 0;
        FillRect(di->hDC, &di->rcItem, GetSysColorBrush(selected ? COLOR_HIGHLIGHT : COLOR_WINDOW));
        SetBkMode(di->hDC, TRANSPARENT);
        SetTextColor(di->hDC, GetSysColor(selected ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT));

        std::wstring text;
        std::shared_ptr<const phantom::Index> ix = CurrentIndex();
        if (const phantom::Rec* r = ix ? ix->RecAt(0, di->itemID) : nullptr)
            text = L"[" + StringToWstring(ix->sys[r->sys].key) + L"] " + StringToWstring(ix->Title(*r));
        RECT rc = di->rcItem;
        rc.left += 3;
        DrawTextW(di->hDC, text.c_str(), (int)text.size(), &rc, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
        return TRUE;
    }

    case WM_DESTROY:
        g_scanStop = true;
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
    // --no-firewall: a second copy run for testing must not raise the elevation prompt again.
    if (!wcsstr(lpCmdLine, L"--no-firewall")) EnsureFirewallRules();
    LoadCachedLibrary();     // the cabinet can be answered from the first moment
    ToggleDaemon();
    StartLibraryScan(false); // and anything new is found behind it, without freezing this window

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return (int)msg.wParam;
}
