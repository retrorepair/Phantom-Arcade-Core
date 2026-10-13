# Handoff — 2026-10-13

This session: the launcher stopped taking the screen from a game that is still being played
(verified on the MiSTer), the daemon's "is a game running" check was fixed after the first real
exit exposed it, FBNeo was built from the fork for the first time, and every build that exists
was deployed and put on GitHub as a release. Everything below is committed and pushed.

## State of the repos and releases

All verified against `retrorepair`, not `origin` — in the emulator forks `origin` is
**verbst's** repo and cannot be pushed to (403).

| Repo | Branch | Head | Release `v2026.10.13` asset |
|---|---|---|---|
| Phantom-Arcade-Core | `main` | `1d92577` | **`v2026.10.13.2`**: `MiSTer_phantom`, `PhantomArcadeManager.exe`, `Phantom_Arcade.rbf`, `SHA256SUMS.txt` (`v2026.10.13` and `.1` are marked superseded) |
| retrorepair/dolphin | `groovy-nlc` | `b9d320863` | `Dolphin-GroovyNLC-…-win64.zip` |
| retrorepair/86Box | `groovy-nlc` | `5593d3c24` | `86Box-GroovyNLC-…-win64.zip` |
| retrorepair/pcsx2 | `groovy-upstream-sync` | `a445baeb2` | `PCSX2-GroovyNLC-…-win64.zip` |
| retrorepair/flycast-dojo | `groovy-rio-buffer-lifetime` | `c07861d6b` | `Flycast-Dojo-GroovyNLC-…-win64.zip` |
| retrorepair/rpcs3 | `groovy-upstream-sync` | `b98401e09` | `RPCS3-GroovyNLC-…-win64.zip` |
| retrorepair/fightcade-fbneo | `groovy-rio-buffer-lifetime` | `e27e0ebf9` | **`v2026.10.13.1`** `FBNeo-Fightcade-GroovyNLC-…-win32.zip` (`fcadefbneo.exe` only; `v2026.10.13` superseded) |
| retrorepair/xemu | `groovy-upstream-sync` | `c5d840daa` | **none** — built, not validated, see below |

Each zip is built from that fork's own build output, not from the deployed folder (which has
the user's configs); the five that existed already were checked as newer than every tracked
source file, PCSX2's even embeds its commit hash. Dolphin was rebuilt from clean HEAD (the
version string was `-dirty`). RetroArch was dropped by the user and has no release; its local
commit (`4848fa7fd`, the RIO fix) is not pushed anywhere.

**Fightcade is two programs.** `fightcade-fbneo` is only the emulator (`fcadefbneo.exe`); the
Fightcade client (lobby/matchmaking) is separate, not in the repo, and launches the emulator with a
`quark:` argument. Nothing in the client needs changing. `ggponet.dll` and the other DLLs come
from the client's `emulator\fbneo` folder, not the repo. See `memory/fightcade-client-is-separate.md`.

## Deploy state (verified by md5 against the repo)

- **MiSTer card:** one `/media/fat/MiSTer_phantom` (current), one `_Utility/Phantom_Arcade.rbf`
  (the unmodified upstream bitstream). `MiSTer_phantom.prev` removed. The core is loaded now; the
  binary is exec'd on every core load, so **no reboot is needed** — an earlier note saying otherwise
  was wrong. Standing rule from the user: leave the card clean and startable from the menu, not
  dependent on my scripts, and release every build — `memory/deploy-standard-user-reachable.md`.
- **PC:** `C:\PhantomArcade\PhantomArcadeManager.exe` = `build_output\` = release asset. The
  daemon is running (started for testing).
- **FBNeo:** `C:\Emulators\fbneo\fcadefbneo.exe` is the new build, with Fightcade's DLLs copied in
  (from the official package, never run) and `config\fcadefbneo.ini` seeded with
  `bGroovyEnabled 1` / `szGroovyHost 192.168.1.204`. The verbst exe is `fcadefbneo.exe.verbst`.
  Groovy output has no command-line switch; it is ini-only.
- **xemu:** the deployed exe is verbst's, put back after a failed test of the new build.

## What landed

### The core holds the screen for a game that is still being played
The user will not accept even a flash of the launcher mid-match. When a stream ends (CMD_CLOSE or
the keepalive timing out at 5 s) `phantom_session_ended()` waits >= 2.5 s, asks the daemon
`STATUS`, and holds the picture up to 20 s only if it answers RUNNING. **Tested on the MiSTer**
with FBNeo streaming Super Street Fighter II Turbo: freezing the process for 10-19 s logs
`[TIMEOUT][no client activity 5000ms][holding the screen]`, no `[MODE][720x480…]` launcher lines
appear, and the stream carries on when it resumes; a clean exit returns to the launcher in
seconds. **Not looked at by eye:** what the CRT showed during the hold. The core log is
block-buffered, so lines can appear well after the event.

### The daemon's STATUS was wrong on the user's machine, and is fixed
It counted any configured emulator whose process was running. With RPCS3 left open at its game
list, a clean exit from another game held the dead game's last frame for the whole 20 s. It
now counts an emulator only if it owns a UDP socket (`GetExtendedUdpTable`): a process streaming to
a MiSTer always does (FBNeo: two), an idle one never (RPCS3: none). The bias is deliberate — a
wrong "running" costs a held frame, a wrong "idle" costs the launcher flashing mid-match.
`host/emulator_status_test.cpp` has 15 checks against real processes and a real socket.
Residual: an emulator that is open *and* holds a UDP socket while idle still counts, so the launcher
can take up to 20 s to return then. See `memory/hold-the-screen-daemon-status.md`.

### FBNeo keeps its EEPROM in local play (`e27e0ebf9`)
Fightcade's emulator never loaded or saved the EEPROM (`@FC: Disable local save of EEPROM`), so CPS2 service-menu
settings (demo sounds...) reset on every launch. They are now saved to `config\games\<game>.nv` and loaded again unless
`is_netgame_or_recording()` (netplay, or recording/playback from power-on), decided once in `EEPROMInit`. Checked: a clean
exit writes a 128-byte `ssf2t.nv` and a second launch reloads it unchanged. **Not checked:** a changed option surviving a
relaunch, and the netgame guard in a real match. Deployed to `C:\Emulators\fbneo`; the user's Fightcade copy
(`C:\Users\joelw\Documents\Fightcade\emulator\fbneo`) still has the previous exe.

### FBNeo never started from Phantom, now does
The daemon passed FBNeo the ROM's full path; FBNeo compares each argument with every driver's short
name for an exact match, so a path gives its "not supported" error window and no game. The FBNeo row now
passes `{rom_stem}` (`ssf2t`). Found because the user tried it; it had never run before because no FBNeo ROM
existed on this machine. Verified end to end: the MiSTer's own `LAUNCH:fbneo_ssf2t` datagram (sent from the
MiSTer over SSH, `python3 -`) was ACKed and `fcadefbneo.exe ssf2t` came up streaming. The other rows were
not re-checked; launches use the compiled defaults, not `phantom_config.json` (which only records them).

### FBNeo builds, and the build is Visual Studio
`7df342987` untracks 2094 `obj/` files the previous commit swept in with `git add -A`. The MinGW
makefile is not a way to build this tree (no Lua, Quark/GGPO, overlay, detectors, TMS34010/i4x00,
ws2_32 — hundreds of undefined references). What works: VS 2022 v143 x86, perl + nasm, the
DirectX SDK June 2010 headers/libs (extracted with 7-Zip from the installer into
`C:\Users\joelw\dxsdk`, injected with an MSBuild `ForceImportAfterCppTargets` file), a clone of
`fightcadeorg/fightcade-detectors` next to the repo (`C:\Users\joelw\Documents\groovy-forks\fightcade-detectors`,
rev `b9e0e40`, 314 detectors compiled in — without them round counters and ranked reporting silently
stop), `run_scripts.bat`, then `msbuild fbneo_vs2015.vcxproj /p:Configuration=Release /p:Platform=Win32`.
**Compare section sizes with a known-good exe**: a first build with a stale generated driver list
linked fine and was 450 KB of code short. Full recipe and the traps (`NoDefaultCurrentDirectoryInExePath`,
WSL's `bash.exe` shadowing MSYS bash) in `memory/windows-build-gotchas.md`.
Tested: starts and runs ssf2t with Fightcade's DLLs, and **streams to the MiSTer** (session open,
~60 fps, `sendFailed=0`, the RIO fence fired once with no timeout). Not tested: a netplay match.

### xemu: built, not released
`c5d840daa8`, version `0.8.136-0-NLC-v1.0-26-gc5d840daa8`, Groovy compiled in; packaged in
`groovy-forks\xemu\dist\` (stripped exe, 26 DLLs including libslirp from the build tree and curl's
transitive chain — `get_deps.py` only copies direct imports, which is what produced the
missing-DLL dialogs in the first test). It has not run successfully here: **it crashes at start,
and so did the deployed verbst build in one run, at the same address inside `nvoglv64.dll`
(`+0x10C3E2D`, `0xC0000409`)**. At the same time the stock build ran at 7-37 fresh frames/s with
the NVIDIA GPU sitting in P8 at 300-400 MHz of 2100. That is the NVIDIA driver in a bad state, the
same shape as the earlier slideshow that a laptop reboot cleared; network (wired GbE, 0 ms), CPU
and leftover processes were all fine. **Next: reboot, then A/B old vs new on Halo 2** (scripts
`run-xemu-gpu.ps1` and `minidump_exc.py` are in `tools/emulator-builds/`; the logic is in
`memory/xemu-nvidia-driver-bad-state.md`) and release if the new build is no worse. The new
build also logs `win32_dxgi_present: FBO incomplete (status=0x8CDD)` and falls back to
`SDL_GL_SwapWindow`. I made no power or GPU setting changes. If a reboot does not help, the
NVIDIA Control Panel's "Prefer maximum performance" for `xemu.exe` is the user's to set.

## Open

- **xemu**: the reboot test above, then a release.
- **On-CRT check** of the hold: nobody has looked at the CRT while a stalled client was held.
- **FBNeo audio stutter: not reproducible, parked.** A tester (D.Owls) reported it through Fightcade; the user
  heard none standalone (44.1 kHz) or through Fightcade, in training mode or Test Game, and the tester never
  replied. Fightcade starts the emulator two ways (from its own log): training mode
  `<game> fbneo-training-mode\fbneo-training-mode.lua savestates\<game>_fbneo.fs` (offline, Groovy-paced) and Test
  Game `quark:served,<game>,<id>,7001,0,null` (the netplay path, `kNetGame=1`). Facts worth keeping from the
  FPGA source (`rtl/sound.v`): "Desired buffer" is only the pre-roll before playback starts, nothing refills it;
  `sound_write_ready` is declared but never used, so the FIFO is never throttled; a starved FIFO makes it drop
  an equal amount of the next audio unless `vram_synced & !vga_frameskip`; and 44.1 kHz audio drains at
  24.576 MHz/562 = 43,729/s, 0.84% slower than sent (it did not show up as a problem). If it returns,
  get the tester's `config\groovy.log` at Log level 1 and the exact command line first.
  Latest from the tester (21:31): Super Street Fighter II Turbo, heard "most in voices" ("try Dhalsim and throw a
  few fireballs"); the Phantom build is still affected but "not nearly as bad" as stock NLC. They start matches from the
  Fightcade client on the PC while the Phantom core is loaded on the MiSTer (the daemon running); Phantom does not start
  Fightcade. The user could not reproduce it in training, Test Game or standalone, so this waits on the tester's
  log and a reproduction; voice-heavy moments (Dhalsim's fireball call) are the thing to try.
- **GameCube/Wii disc titles**: the disc header at 0x20 has the real title, like Xbox. Offered,
  not answered.
- **Unify the vendored Groovy client.** All copies now carry `rioFence`, but they still diverge:
  PCSX2's has `gmw_set_keepalive` and `rioServiceQueues()`; 86Box's lacks both and alone carries an
  `<algorithm>`/`<limits>` fix it needs under MinGW. 86Box has no keepalive (harmless: it does not
  advertise `GM_CAP_KEEPALIVE`).
- **Jumbo frames** untested: core side ready; needs the OSD option, client MTU 3800 and a
  jumbo-capable path.
- **Sunshine needs one click** — an NKit image and Dolphin's warning dialog blocks batch mode.
  Tick "Don't show this again" or set `SkipNKitWarning = True` under `[Interface]`.
- The FBNeo fork's `makefile.mingw` fixes in `7070c01` are real defects but only get it as far as
  the link; the VS build is the supported one.

## Where things live

Forks under `C:\Users\joelw\Documents\groovy-forks\` except rpcs3
(`C:\Users\joelw\Documents\rpcs3-groovy`), deployed to `C:\Emulators\<name>\`. MiSTer is
`192.168.1.204`; its SSH host key is not in PuTTY's cache, so pass
`-hostkey SHA256:FqNJOsj3FLUoMQxgn+cqGoXvVfENmVK4QFoSCMKl2lU` to plink/pscp or it hangs. Load the
core for a test with `echo "load_core /media/fat/_Utility/Phantom_Arcade.rbf" > /dev/MiSTer_cmd`;
the core log is `/tmp/groovynlc.log` (absent until the core has been loaded). Do not `find` the
whole card without a timeout — the games folder takes minutes.

**The launcher the user runs is `C:\PhantomArcade\PhantomArcadeManager.exe`** — not
`build_output\`, which is only the repo's shipped copy of the same build. That folder holds the
user's own `phantom_config.json`, `mame_titles.txt` and `mister\`, so deploy by copying the exe over
the top and leave everything else alone. The daemon serves `games_catalog.json` from the folder
its own exe is in, so two copies means two catalogs; search the whole drive before declaring there
is only one. Its startup scan takes ~85 s, and the cabinet only re-fetches when the host goes from
offline to online, so restart the launcher to push a new catalog.
