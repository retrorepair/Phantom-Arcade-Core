# Handoff — 2026-10-13

This session: the launcher stopped taking the screen from a game that is still being played
(verified on the MiSTer), the daemon's "is a game running" check was fixed after the first real
exit exposed it, FBNeo was built from the fork for the first time, every build that exists was
deployed and put on GitHub as a release - and then the launcher was reworked for TVs that
overscan (screen-adjust page, MiSTer-standard button colours and controls) and for libraries of
hundreds of thousands of games (paged catalog, fast incremental scan). Everything below is
committed and pushed; the last release is `v2026.10.14.1`.

## State of the repos and releases

All verified against `retrorepair`, not `origin` — in the emulator forks `origin` is
**verbst's** repo and cannot be pushed to (403).

| Repo | Branch | Head | Release `v2026.10.13` asset |
|---|---|---|---|
| Phantom-Arcade-Core | `main` | see `git log` | **`v2026.10.14.1`**: `MiSTer_phantom`, `PhantomArcadeManager.exe`, `Phantom_Arcade.rbf`, `SHA256SUMS.txt` (every earlier `v2026.10.13*` is marked superseded) |
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

- **MiSTer card:** one `/media/fat/MiSTer_phantom` (md5 `e0fb484c8dda`, = `build_output\` = the
  `v2026.10.14.1` release asset), one `_Utility/Phantom_Arcade.rbf` (the unmodified upstream bitstream); the
  only other thing is `MiSTer.ini.prephantom`, a backup of the user's own ini. `phantom.ini` holds
  the user's saved screen margins (`SAFE_L/T/R/B`) - leave it alone. `config/phantom_bg.ppm` is their
  chosen background picture (see above).
  **Deploying a new `MiSTer_phantom` over a running one reboots the board when the core is reloaded**
  (seen five times). Not a launcher bug: `fpga_load_rbf` ends in `app_restart`, which `execl`s
  `/proc/self/exe`; after `mv` replaces the file that link reads `... (deleted)`, the `execl` fails and
  Main_MiSTer takes its own "Something went wrong. Rebooting..." path. It lands at the menu; load the
  core from there (`load_core` from the menu state is fine, and a reload with nothing replaced is
  harmless). An earlier note here saying "no reboot is needed" was wrong. `/tmp` on the board is
  cleared by that reboot. Standing rule from the user: leave the card clean and startable from the menu, not
  dependent on my scripts, and release every build — `memory/deploy-standard-user-reachable.md`.
- **PC:** `C:\PhantomArcade\PhantomArcadeManager.exe` (md5 `38f28e1107f3`) = `build_output\` = the
  `v2026.10.14.1` release asset. The daemon is running with the user's own `phantom_config.json` (checked
  byte-for-byte after a test that swapped it for a synthetic-library config and back). New files it
  keeps beside the exe: `phantom_index.bin` (the last scan), `fbneo_sets.cache` (FBNeo's supported
  list, keyed by its exe), `mame_rejects.txt` (sets MAME rejected, keyed by a fingerprint of its
  folder). `games_catalog.json` / `games_catalog_cabinet.json` there are stale leftovers; nothing
  reads them any more.
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

## 2026-10-14: why FBNeo, Dreamcast and PS3 "never loaded", and what was done

The user reported that FBNeo, Dreamcast and PS3 games left the cabinet on "Host is starting the emulator..." for ever. Three
separate causes, found by launching each through the daemon's own protocol and watching the process and its windows:
- **Dreamcast (a regression from the new scanner):** a game with a `.7z` beside a folder holding its `.cue` got the `.7z`
  (tie-break was the lexicographically smallest path, `.` sorts before `/`), and Flycast quits silently, exit code 0, on that
  archive. The old scan got it right only by directory order. Now a loose disc file or descriptor beats an archive
  (`archiveRank` in `library_scan.h`, tested, and shown to fail with the rule disabled).
- **FBNeo:** it only looks in its own `szAppRomPaths`, which on this machine were empty, so only `C:\Emulatorsbneo\ROMs`
  (holding just `ssf2t.zip`) was searched; the library comes from `Y:\MiSTer\mame`. The dialog said "<game> was not found.
  ... No data was found". The daemon now adds the configured ROMs folder to the first empty slot of FBNeo's ini before
  launching (`EnsureFbneoRomPath`; never overwrites a path the user set).
- **PS3:** RPCS3 refuses to run twice ("Another instance of RPCS3 is running"), and the user's RPCS3 had been open at its game
  list since Oct 11. The daemon now closes other RPCS3 instances (WM_CLOSE, then terminate) before launching one
  (`CloseRunningInstances`). Note this closes an RPCS3 window the user may have been using.
And the reason they hung rather than failed: the PC knew (it said so in its own window) but never told the cabinet. Now the
daemon remembers who sent the LAUNCH and sends `LAUNCHFAIL:<line>` when the emulator does not start, stops within 14 s, or shows
an error window (a `#32770` dialog, or a title ending "Error" / containing "Fatal Error" - not just "Fatal", which is a game);
the cabinet leaves the loading screen and shows the line for ~11 s. A process that owns a UDP socket is streaming and ends the
watch early. Verified on the real setup: Crazy Taxi and `zzyzzyxx` launch and stream, WipEout HD launches with RPCS3 open, and
`assault` (a set whose ROMs are incomplete) is reported within 0.3 s as "FBNeo (Fightcade) is showing an error window".
Open: a FBNeo ROM **audit** (my prototype `fb_audit.py` reads each zip's CRCs against the `-listinfo` DAT and the `romof`
chain: 1,177 of the user's 1,217 listed sets would load, 40 would not) so unloadable sets are not listed at all - not built.
Also found while testing: Windows showed a Firewall prompt for my own test program `bm_test.exe`; the user should Cancel it.

## What landed later in the session: overscan, controls, and large libraries

### The launcher fits TVs that overscan, and uses MiSTer's own button colours
The user's TV hid the top third of the title and everything below the info bar. The layout now has a
per-edge safe area (`ph_safe`, defaults 36/24/36/24, saved as `SAFE_L/T/R/B` in `phantom.ini`) set on a
screen-adjust page: **Select** opens it, Left/Right picks an edge, Up/Down moves it (Up shrinks the box),
A saves, B cancels, Y resets. Backgrounds still fill the raster; text and the legend stay inside. The
legend is A red, B yellow, X blue, Y green with L/R/L2/R2/Select as key caps, and **the pad bits were
wrong before**: `user_io.h`'s `JOY_SELECT`/`JOY_START` (0x40/0x80) are the X/Y positions in this core, so
"refresh" was Y and the exit hotkey was X+Y. The core's ten positions are A, B, X, Y, L, R, Select, Start,
L2, R2 = bits 0x10...0x2000 (confirmed from the saved pad map). The exit hotkey is now Start+Select.
Controls: Up/Down move; Left/Right page (held: accelerating); L/R platform; L2/R2 letter; A launch; B back
out of a launch; Y rescan+refresh; Select screen adjust. **The user confirmed the screen looks right on
their TV.**

### Libraries of hundreds of thousands of games
Why: the catalog was one JSON file (rewritten per scan, searched as text per launch) sent to the cabinet in
one 60,000-byte datagram into `PH_MAX_GAMES 512`, and MAME was asked about every set on one command line
(Windows stops at ~32,000 characters, so a few thousand sets failed and the filter silently switched off).

PC (`host/`, header-only and portable except the walk): `game_index.h` (the index; wire format documented at
its top; 300,000 games ~36 MB, built sorted in ~0.5 s, a page in ~13 us), `rom_enum.h` (FindFirstFileEx walk,
~4x `std::filesystem`), `library_scan.h` (incremental scan; services for MAME/PARAM.SFO/XBE come in as
callbacks so the test can count calls). Cabinet: `phantom_cat.c/h` (the paged client, pure C),
`phantom_ui` row-source interface + placeholders, `phantom.cpp` driving it. Protocol, version/STALE
semantics and launch-by-position are in the README's "Large libraries" section.

Behaviours worth knowing: unchanged rescan asks MAME zero times; if anything under the MAME folder changes,
rejected sets are asked again (a missing parent may have appeared); a rescan that finds nothing new does not
bump the version (`IndexEquivalent`, which includes size and mtime); the cabinet notices a rescan on the PC
within ~10 s or at the next page it lacks; a launch quoting an old version gets `ACK:LAUNCH:STALE` and the
cabinet says "The library changed - choose again"; an older PC is served the old way after ~1.5 s and a PC
replaced by a different build (older or newer) is noticed without pressing anything. Arcade set names that
differ only by case are one game (`kinst.zip` and a `Kinst` folder).

**Measured, on this PC and MiSTer:** 300,000 files walked in ~0.25 s; a scan with nothing changed ~0.4 s; a
260,000-title synthetic library (empty files with plausible names in `%TEMP%\pa_ps2_260k`, made by
`make_big_library.py`, not kept in the repo) listed within 1.2 s of the daemon starting, 81 MB resident,
incremental rescan ~1 s; on the MiSTer five letter jumps landed on exactly the host's table
(70991/260001), 30 fast mixed keys left all 13 rows loaded, `LAUNCH:@ver:*:index` resolved to the right id
(the FBNeo command line was `fcadefbneo.exe <that game>`), a real FBNeo launch streamed, and stopping it
returned to the launcher at the same cursor. The real library lists 1,258 games, the same set the old daemon
listed (checked id by id, zero field differences).

How the hardware was driven (reusable): `tools/mister_fbdump.py` reads the launcher's canvas out of DDR
and `tools/mister_inject.py` presses keys through uinput (both copied to `/tmp` on the board over scp, they
vanish on reboot; keys now include `pgup pgdn comma dot q e f2`, and `--settle 0.5` no longer parses as a
key). A test copy of the daemon cannot serve the MiSTer from `%TEMP%`: only the exact path
`C:\PhantomArcade\PhantomArcadeManager.exe` has an inbound firewall rule, so the scale test ran the real
daemon with a swapped config and put everything back.

Side findings: the daemon used to add a firewall rule (with an elevation prompt) on **every start** -
there are 43 "Phantom Arcade Manager" rules, 6 of them for the live path; it now looks first, but the
duplicates are the user's to clean up (needs admin). `--no-firewall` skips the check for a test copy.
`tools/phantom_input_test.cpp` now has a fake host that speaks the paged catalog using the daemon's own
`game_index.h`, so each side is tested against the other's real code.

Not done / ideas: the cabinet shows no per-tab counts in the tab strip; there is no type-to-search; MAME's
own first-time verification of a huge new set is as slow as `mame -verifyroms`; the user asked, for fun, about a
dimmed game snapshot behind a translucent menu and downloading title/snap images per game. **The look is on the cabinet (v2026.10.14).**
`phantom.cpp` loads `/media/fat/config/phantom_bg.ppm` (a 720x480 P6 PPM) whenever the menu comes up, if it is there and has
changed (modification time to the nanosecond, so replacing it needs no restart); `ph_ui.bg` is the picture dimmed with
`ph_ui_dim`, the canvas starts as it, and every panel is a translucent layer over what is already drawn (`rect()` blends when
`ph_ui.alpha` is set; text, the amber rules, the cursor and the button badges are solid; the screen-adjust page ignores the
picture). Header/tab strip/detail line/legend are 200/255 opaque (`PH_BG_PANEL_ALPHA`), the game list 102/255 = **60%
see-through, which is what the user settled on** (`PH_BG_LIST_ALPHA`; they went 60%, then 50%, then back to 60%). `phantom.ini [LAUNCHER]`
takes `BG_DIM` (default 50), `BG_PANELS`, `BG_LIST`, and `phantom_save_config()` writes them back so a save does not drop them.
**What is on the user's card now:** their own Super Street Fighter II Turbo ranking screen, captured from the running emulator
window on this PC (GetClientRect + CopyFromScreen, 720x480), at `/media/fat/config/phantom_bg.ppm` - persistent across
reboots, which is what they asked for ("just make it persistent for now"). The picture is not in the repo (it is a game frame).
`tools/make_bg.ps1` makes the file from any image. **Cost on the HPS, measured with `tools/phantom_render_bench.c` built for
the A9 and run on the board:** 7.0 ms a frame flat, 11.3 ms over a picture, against a 16.7 ms field; it only runs on redraw and
no stream is running while the menu is up. Tests: 10 renderer checks (`test_background`) and harness section 9 (the real
launcher reading a picture from disk: appears dimmed, a replaced one is picked up, a wrong-size file is ignored and the held
one stays, removing it restores the flat menu, launching still works); the harness stub now keeps the last presented frame.
**Approved on 2026-10-14 ("definitely continue with the title/screen fetching"): fetching titles/screens per game.** That is the
top item. Its design and status are in the "Artwork" section. misterzine's images come from a private CDN
(`images.misterzine.fyi/snap/<mame set>.png`, 320x237) - **not used**; the sources are libretro-thumbnails, ProgettoSNAPS,
and PS3 `ICON0`/`PIC1`, each fetched only with the user's say-so (given for titles and screens in general; ScreenScraper
needs the user's own account, which I cannot create).

## 2026-10-14 (late): PS2 "frames out of sync in chunks", and the review of Phantom-Arcade-Core PR #1

### PS2 tearing: what could and could not be established
The user saw PS2 (PCSX2) frames "vertically out of sync in chunks, like vsync off on a PC game" and asked for screenshots.
Stream at the time: Burnout 3, 640x448, `interlace=2 fields-from-core`, NLC (`Codec=7`), `Pacing=0` (PCSX2 is the clock),
`Readback=0`. Evidence gathered, in the order it was obtained:
- **DDR snapshots** (`tools/mister_burst.py` + `tools/analyse_tearing.py`): 55 of 60 captures changed all the way down, 4 partly.
  Inconclusive: an uncached read of the frame takes ~39 ms, so it cannot catch a few-millisecond tear.
- **The core's own sync-loss counters** (`/tmp/groovynlc.log`, verbose 0): no `[SYNCLOSS]` and no `sync=0` status line over several
  minutes. The core never ran out of pixels for the raster.
- **Frame-number log** (`tools/mister_framelog.py`; the compressed-blit header is at DDR byte 32): 60.00 frames a second, every
  step exactly +1 over 1800 frames - nothing dropped, repeated or reordered. But the *arrival* times are bunched: of 1800 gaps,
  ~150 under 8 ms and ~105-125 over 26 ms around the 16.68 ms mean (worst ~32 ms).
- **A/B of PCSX2 `Pacing` 0 -> 1** (MisterMaster, the CRT as the clock): the same histogram. So the jitter is not PCSX2's frame
  limiter; it is introduced after it (encode time, the PC's send path, the network, or the HPS ingest). The setting was put back to 0.
Reading `Groovy.sv`: with OSD "Volatile framebuffer: Off" (`hps_frameskip`), a frame that has not arrived when the raster needs
its lines is replaced by the *previous frame's* lines from the framebuffer (`cmd_fskip` auto-blit), and the new frame is picked up
mid-field when it lands (`PoC_frame_vram`, "frame arrives soon, finish blit last asap"). A late frame therefore shows as old above
a line and new below it - the user's description. The 15% of frames that arrive 10+ ms off their slot fit that. **That is a
hypothesis, not a measurement**: nothing here observed `vga_frameskip` itself (it is only logged at verbose 1+, which is an OSD
option; the HPS could count its rising edges and log them next to `[HKGAP]` - the cheapest way to settle it). Cheap things for
the user to try on the CRT: OSD "Volatile framebuffer: On" (late frames then show as a gap instead of a tear) and "Vsync overlay: On".
Ideas if it is confirmed: jumbo frames (the client is at MTU 1500, so a frame is ~70 datagrams), a larger "Framebuffer lead", a
sender-side pacing point that sends each frame at a fixed phase of the CRT's field instead of when the encode finishes.

### Review of retrorepair/Phantom-Arcade-Core PR #1 (browser frame monitor)
Draft; adds `api/browser_monitor.h` (240 lines), a hook in `CmdBlit` and `docs/BrowserMonitor.md`; serves the pre-compression
frame on `http://<pc>:8765/` when `GM_BROWSER_MONITOR=1`. I built and ran its header against a synthetic 720x480 stream
(`scratchpad/pr1src`, `bm_test.cpp`, `bm_client.py`). **It will not work as submitted:**
1. The page asks for `/frame.bmp?t=<ms>` (a cache-buster) but the handler matches `"GET /frame.bmp "` with the trailing space,
   so every request falls through to the 404 branch and the page sits on "Waiting for first frame" forever. Parse the path and
   drop the query string.
2. The server is one thread that does a blocking `recv()` on each client in turn with no timeout. A browser's speculative
   pre-connect (or anything that connects and says nothing) blocks it for good. Needs a receive timeout or a thread per client.
3. Red and blue are swapped: the wire format for RGB888/RGBA8888 is B,G,R (`GroovyMiSTerPixels.cpp`: `out[0] = in[2]`), the
   converter reads R,G,B. (RGB565 is right.)
4. In true-field mode (`interlace=1`) `m_RGBSize` is halved, so the monitor shows half-height fields; at 20 fps (every third
   frame) they alternate parity and the picture bobs a line up and down. It needs weaving, or a fixed parity.
Also: it is per-emulator-process, so every emulator exe needs its own inbound firewall allow (rules here are per exact path -
the "user faff" the user wants to avoid) and two emulators fight over port 8765 (`SO_REUSEADDR` on Windows lets a second
process bind the same port; use `SO_EXCLUSIVEADDRUSE`); the env var would have to be set by whatever starts the emulator; a
32-bit BMP at 20 fps is ~28 MB/s on the LAN (JPEG via a single-header encoder, or a lower rate, is kinder to Wi-Fi); it binds
`0.0.0.0` with no access control; and it shows what the PC *sent*, not what the CRT shows, so it cannot show the Phantom menu,
a held frame, a failed launch, or a tear.
**Recommended integration (least faff): serve the monitor from the MiSTer instead.** `MiSTer_phantom` already maps the core's
DDR frame (what `tools/mister_fbdump.py` reads), knows the mode, and runs for every emulator and for the menu. It would answer
`http://<mister>:8765/` with the PR's page and a frame fetched only while a viewer is connected, capped at a few frames a second
(an uncached read of a whole frame is ~39 ms, so it costs the HPS and the DDR arbiter noticeable time; keep it off the hot
thread and off by default, switchable from the new Phantom settings menu). No PC firewall rule, no environment variable, no
emulator change, and it shows the true CRT frame - which is what the tearing question needed. The PC-side tap is still worth
keeping, off by default, as the *pre-compression* source for encoder A/B (compare PC frame with CRT frame). Order: after artwork
and the settings menu; fix items 1-4 either way before merging.

## Artwork and the settings menu (2026-10-14)

The user approved fetching titles/screens ("definitely continue") and asked for a settings menu on **Select** with, at minimum,
what to fetch and which to display, the screen-boundary setting inside it, and the list at 60% see-through (settled).

**What was built**
- **PC** (`host/`): `art.h` (name rules, listing parser, the match ladder, folders per system, URLs, 4:3 fitting and
  resampling - portable), `art_store.h` (disk cache, worker threads, priorities, back-off, whole-library run - portable,
  HTTP/decoder injected), `art_http.h` (the `/art` request and the `ART:` datagrams - portable), `art_win.h` (WinHTTP + WIC).
  The daemon's HTTP server (TCP 8088) was rewritten: one thread per connection with receive timeouts (it was a single thread
  with a blocking read, so one silent client stopped everything - the same flaw found in the browser-monitor PR), routes
  `/catalog.json`, `/` and `/art`. `--http-port N` lets a test copy run beside the real daemon. The "what may be downloaded"
  setting lives in `art\settings.txt` (not `phantom_config.json`, which is rewritten from the window's controls).
- **Cabinet** (`hps_linux/src/support/groovy/`): `phantom_art.c/.h` (non-blocking HTTP client: debounce 250 ms, one GET, a
  megabyte over many polls, retries while the host says 202), a settings page in `phantom_ui.c` (`PH_VIEW_SETTINGS`; the model
  is filled by `phantom.cpp`), and in `phantom.cpp` the picture logic (the fetched picture wins over `phantom_bg.ppm`, which
  stays the fallback; both kept undimmed so brightness changes live), `ART_SHOW=off|title|snap` in `phantom.ini`, the
  settings model and its input, and `ART:INFO` polling while the menu is open. The adjust page now returns to wherever it was
  opened from.
- **Wire**: `GET /art?v=&l=&i=&k=title|snap` -> 200 (720*480*3 B,G,R), 202 (fetching, ask again), 404, 409 (library changed),
  400; `ART:INFO` / `ART:SET:<mask>` / `ART:ALL:<1|0>` over UDP. A 200/202 also prefetches the rows around it. The discovery
  reply is `PHANTOM_HOST_ONLINE:<udp>:<http>` (the cabinet reads the HTTP port from it).

**Source and matching.** libretro thumbnails (`thumbnails.libretro.com/<system>/Named_Titles|Named_Snaps/<name>.png`), named
after the game as No-Intro/Redump/MAME/FBNeo spell it with `&*/:` etc. replaced by `_`. There is no search: the folder listing
is the index, fetched once per system and kept (two weeks). Ladder: the same name -> the file name (Redump names for discs) ->
same name ignoring case/punctuation/"Rev"="Revision"/a moved "The" -> the same game's other release, preferring the one whose
bracketed words agree, then World > USA > Europe > Japan. On the live library: FBNeo 1,210 of 1,217 titles match, MAME 24 of 30
(the rest are renamed or absent on the server); a whole-library download fetched 1,242 titles and 1,243 screenshots (64 MB,
about 4 minutes at 3 requests at a time with 150 ms between each). 86Box machines and unknown systems have no pictures.
misterzine's CDN (`images.misterzine.fyi`, private) is **not** used. ScreenScraper (needs the user's own account) and
ProgettoSNAPS packs are not used; libretro covered the library.

**Found while building it:** the PS3 `Named_Titles` folder on the server is empty (a valid empty index - handled as "no pictures"
and kept, not as an outage); `windows.h` min/max macros clash with `std::min` (the headers parenthesise it); a Bash tool
`/c` argument is rewritten to `C:\` by MSYS (use `cmd.exe //c`, and `MSYS_NO_PATHCONV=1` for `wsl -- bash /mnt/...`).

**Not done / next:** a crossfade between pictures (it is a hard cut); ScreenScraper as a second source for what libretro lacks;
a settings line for the picture dim of panels; per-system choice of title vs screenshot; the FBNeo ROM audit; the HPS-side
browser monitor (see the PR #1 review above).

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
is only one. The scan runs in the background after start (the window is usable and the cabinet is served the last scan
at once); the cabinet picks up a new library within ~10 s, or press Y on it to have the PC look for changes
now.
