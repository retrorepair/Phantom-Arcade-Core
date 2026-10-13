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

#include <windows.h>
#include <tlhelp32.h>

#include <cwctype>
#include <set>
#include <string>

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

// Is any process running whose executable name is in `lowerNames` (lower-case file names,
// no path)? This is what catches a game the daemon did not launch itself - a Fightcade match
// is started by the Fightcade client - which is exactly the case that must not be interrupted.
//
// The price is that an emulator left open and idle also counts. The cabinet bounds the
// damage with a cap on how long it will hold the screen for a game it cannot see streaming.
inline bool AnyProcessNamed(const std::set<std::wstring>& lowerNames) {
    if (lowerNames.empty()) return false;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;

    bool found = false;
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe)) {
        std::wstring n = pe.szExeFile;
        for (auto& c : n) c = (wchar_t)towlower(c);
        found = lowerNames.count(n) != 0;
    }
    CloseHandle(snap);
    return found;
}
