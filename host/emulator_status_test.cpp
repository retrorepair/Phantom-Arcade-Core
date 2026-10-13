// emulator_status_test.cpp - the daemon's liveness checks, against a real process.
//
//   cl /nologo /std:c++17 /EHsc host\emulator_status_test.cpp /Fe:emulator_status_test.exe
//
// Spawns a copy of ping.exe under a name no real emulator uses. It runs until killed, which
// Windows 11's notepad.exe does not (that is a launcher stub that exits at once), and a pass
// means the match is by the name we asked about, not by whatever else is open on the machine.
// Exit status is the number of failed checks.
#include "emulator_status.h"

#include <cstdio>

static int g_fail = 0;
static void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) g_fail++;
}

int main() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring probe = std::wstring(tmp) + L"emu_status_probe.exe";

    wchar_t sysdir[MAX_PATH];
    GetSystemDirectoryW(sysdir, MAX_PATH);
    if (!CopyFileW((std::wstring(sysdir) + L"\\ping.exe").c_str(), probe.c_str(), FALSE)) {
        std::printf("cannot stage a probe executable (error %lu)\n", GetLastError());
        return 99;
    }

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    std::wstring cmd = L"\"" + probe + L"\" -t 127.0.0.1";
    if (!CreateProcessW(NULL, &cmd[0], NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        std::printf("cannot start the probe (error %lu)\n", GetLastError());
        return 99;
    }
    Sleep(500);

    FILETIME c, e, k, u;
    GetProcessTimes(pi.hProcess, &c, &e, &k, &u);
    ULONGLONG created = ((ULONGLONG)c.dwHighDateTime << 32) | c.dwLowDateTime;

    std::printf("a running emulator\n");
    check(ProcessAliveSince(pi.dwProcessId, created), "its pid is alive, with the creation time we recorded");
    check(ProcessAliveSince(pi.dwProcessId, 0), "alive with no creation time recorded (just liveness)");
    check(!ProcessAliveSince(pi.dwProcessId, created + 12345),
          "a recycled pid (same number, different creation time) does not read as ours");
    check(!ProcessAliveSince(0, 0), "pid 0 is never an emulator");
    check(AnyProcessNamed({ L"emu_status_probe.exe" }), "found by its executable name");
    check(!AnyProcessNamed({ L"definitely_not_running_anywhere.exe" }), "a name nothing is running under is not found");
    check(!AnyProcessNamed({}), "an empty name list matches nothing");

    std::printf("an emulator that is open but not streaming (no UDP socket) versus one that is\n");
    check(!AnyProcessNamed({ L"emu_status_probe.exe" }, true),
          "the probe is running but owns no UDP socket: not counted when UDP is required");
    check(AnyProcessNamed({ L"emu_status_probe.exe" }, false), "and is still counted when it is not");

    // This process stands in for a streaming emulator: send-only UDP socket, never bound by hand,
    // exactly how the Groovy client's video socket gets its ephemeral port.
    wchar_t selfPath[MAX_PATH];
    GetModuleFileNameW(NULL, selfPath, MAX_PATH);
    std::wstring self = selfPath;
    self = self.substr(self.find_last_of(L"\\/") + 1);
    for (auto& c : self) c = (wchar_t)towlower(c);

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    check(!PidsOwningUdpEndpoint().count(GetCurrentProcessId()), "before it opens a socket this process owns no UDP endpoint");
    check(!AnyProcessNamed({ self }, true), "so it does not count as streaming");
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in to = {};
    to.sin_family = AF_INET;
    to.sin_port = htons(9);                 // discard; nothing needs to be listening
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sendto(s, "x", 1, 0, (sockaddr*)&to, sizeof(to));
    check(PidsOwningUdpEndpoint().count(GetCurrentProcessId()) != 0, "after a send-only UDP socket it owns one");
    check(AnyProcessNamed({ self }, true), "and now counts as streaming");
    {
        auto pids = PidsOfProcessesNamed({ self }, true);
        bool found = false;
        for (DWORD p : pids) if (p == GetCurrentProcessId()) found = true;
        check(found, "PidsOfProcessesNamed returns this process's pid while it has a UDP socket");
        auto probePids = PidsOfProcessesNamed({ L"emu_status_probe.exe" }, true);
        check(probePids.empty(), "and returns nothing for the probe, which has none (so an idle emulator is never killed)");
        auto probeAny = PidsOfProcessesNamed({ L"emu_status_probe.exe" }, false);
        check(probeAny.size() == 1 && probeAny[0] == pi.dwProcessId, "without the UDP requirement it finds exactly the probe's pid");
    }
    closesocket(s);
    WSACleanup();

    TerminateProcess(pi.hProcess, 0);
    WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    Sleep(300);

    std::printf("the same emulator after it exits - the case g_activePid alone got wrong\n");
    check(!ProcessAliveSince(pi.dwProcessId, created), "the pid it had is no longer alive");
    check(!AnyProcessNamed({ L"emu_status_probe.exe" }), "and it is no longer found by name");

    DeleteFileW(probe.c_str());
    std::printf("\n%d check(s) failed\n", g_fail);
    return g_fail;
}
