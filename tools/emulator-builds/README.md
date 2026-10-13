# emulator-builds

Scripts used to build, package and diagnose the GroovyNLC emulator forks on the user's laptop.
Paths are that machine's (`C:\Users\joelw\...`); read them as a record of what worked rather
than as portable tools. The reasoning, and every trap met on the way, is in the handoff and the
session memory notes ("windows-build-gotchas").

| Script | What it does |
|---|---|
| `fbneo-vs.cmd` + `dx.targets` | Build `fightcade-fbneo` with Visual Studio: clears `NoDefaultCurrentDirectoryInExePath`, puts perl/nasm on PATH, runs MSBuild Release/Win32 with the DirectX SDK (June 2010) headers injected |
| `msys-run.sh` | Run a command inside MSYS2 (`MINGW64` etc.) with a Windows home directory set, logging to a file |
| `xemu-ninja.sh`, `xemu-package.sh`, `dll_closure.py` | Finish and package the xemu build: generate the version header with MSYS bash, strip the exe, copy its full transitive DLL set (libslirp comes from the build tree) |
| `run-xemu-gpu.ps1` | Run xemu on Halo 2 with Groovy logging while sampling `nvidia-smi` once a second |
| `minidump_exc.py` | Read the exception code and faulting module out of a Windows minidump without a debugger |
| `suspend-proc.ps1`, `close-proc.ps1` | Freeze a process (NtSuspendProcess) or close its window, for testing the core's hold-the-screen behaviour |
