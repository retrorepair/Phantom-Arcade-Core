// emulator_status.h - is an emulator running right now?
//
// The daemon answers the cabinet's STATUS request with this, and the cabinet uses it to tell
// two quite different things apart when a stream ends: the game is over, or the game is still
// there and the connection blipped. The first should put the launcher back; the second must
// never, least of all in the middle of a match - taking the screen reprograms the CRT's video
// mode and then the client's own modeline reprograms it back.
//
// Header-only and free of the daemon's globals so host/emulator_status_test.cpp can exercise
// exactly what ships.
#pragma once

// winsock2.h has to come before windows.h, or windows.h drags in the old winsock.h and the two clash
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <tlhelp32.h>

#include <cwctype>
#include <set>
#include <string>
#include <vector>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

// Is `pid` a live process? When `createdAt` (a FILETIME as a 64-bit count) is non-zero it must
// also have been created at that instant, so a pid the system has since recycled for something
// unrelated does not read as the process we remember. 0 for createdAt means "any".
inline bool ProcessAliveSince(DWORD pid, ULONGLONG createdAt) {
    if (!pid) return false;

    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;

    DWORD code = 0;
    bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    if (alive && createdAt) {
        FILETIME c, e, k, u;
        if (GetProcessTimes(h, &c, &e, &k, &u)) {
            ULONGLONG got = ((ULONGLONG)c.dwHighDateTime << 32) | c.dwLowDateTime;
            if (got != createdAt) alive = false;
        }
    }
    CloseHandle(h);
    return alive;
}

// The process ids that own at least one IPv4 UDP endpoint. A process streaming to a MiSTer
// always does: the video socket and (if inputs are on) the input socket both live in the
// emulator itself, and a send-only UDP socket takes an ephemeral local port. An emulator that
// is simply open - RPCS3 sitting at its game list, say - owns none.
inline std::set<DWORD> PidsOwningUdpEndpoint() {
    std::set<DWORD> pids;

    DWORD size = 0;
    std::vector<BYTE> buf;
    // The table can grow between the sizing call and the real one, so retry on that error.
    for (int attempt = 0; attempt < 5; attempt++) {
        DWORD rc = GetExtendedUdpTable(buf.empty() ? NULL : buf.data(), &size, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0);
        if (rc == NO_ERROR) {
            const MIB_UDPTABLE_OWNER_PID* t = reinterpret_cast<const MIB_UDPTABLE_OWNER_PID*>(buf.data());
            for (DWORD i = 0; i < t->dwNumEntries; i++) pids.insert(t->table[i].dwOwningPid);
            break;
        }
        if (rc != ERROR_INSUFFICIENT_BUFFER) break;
        buf.resize(size);
    }
    return pids;
}

// The process ids of every running process whose executable name is in `lowerNames`, optionally
// only those that own a UDP endpoint (see PidsOwningUdpEndpoint). This is what lets "stop the
// game" reach an emulator the daemon did not start itself - a Fightcade match is started by the
// Fightcade client, so the daemon has no pid for it.
inline std::vector<DWORD> PidsOfProcessesNamed(const std::set<std::wstring>& lowerNames, bool needUdp = false) {
    std::vector<DWORD> out;
    if (lowerNames.empty()) return out;

    std::set<DWORD> udpPids;
    if (needUdp) udpPids = PidsOwningUdpEndpoint();

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;

    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        std::wstring n = pe.szExeFile;
        for (auto& c : n) c = (wchar_t)towlower(c);
        if (lowerNames.count(n) && (!needUdp || udpPids.count(pe.th32ProcessID)))
            out.push_back(pe.th32ProcessID);
    }
    CloseHandle(snap);
    return out;
}

// Is any process running whose executable name is in `lowerNames` (lower-case file names,
// no path)? This is what catches a game the daemon did not launch itself - a Fightcade match
// is started by the Fightcade client - which is exactly the case that must not be interrupted.
//
// With `needUdp` the process must also own a UDP endpoint, which is what separates a game that
// is streaming from an emulator that is merely open. Without it, any emulator left open and
// idle makes every other game's exit look like a blip and the launcher is held back for the
// full cap. A wrong "running" costs a held last frame; a wrong "idle" costs the launcher
// flashing up in the middle of a match, so a doubtful process is still counted - only one with
// no UDP socket at all is passed over.
inline bool AnyProcessNamed(const std::set<std::wstring>& lowerNames, bool needUdp = false) {
    if (lowerNames.empty()) return false;

    std::set<DWORD> udpPids;
    if (needUdp) udpPids = PidsOwningUdpEndpoint();

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;

    bool found = false;
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe)) {
        std::wstring n = pe.szExeFile;
        for (auto& c : n) c = (wchar_t)towlower(c);
        found = lowerNames.count(n) != 0 && (!needUdp || udpPids.count(pe.th32ProcessID) != 0);
    }
    CloseHandle(snap);
    return found;
}
