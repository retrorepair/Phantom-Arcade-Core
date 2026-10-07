# Handoff — 2026-10-07

Dolphin and 86Box joined the GroovyNLC set this session, and the launcher grew the tabs,
the launch path and the metadata to carry them. Everything below is committed and pushed.

## State of the repos

All verified against `retrorepair`, not `origin` — in the emulator forks `origin` is
**verbst's** repo and cannot be pushed to (403). That is why pcsx2 and xemu looked pushed
for days and were not.

| Repo | Branch | Head |
|---|---|---|
| Phantom-Arcade-Core | `main` | `447816f2d` |
| retrorepair/dolphin | `groovy-nlc` | `b9d320863` |
| retrorepair/86Box | `groovy-nlc` | `5593d3c24` |
| retrorepair/pcsx2 | `groovy-upstream-sync` | `a445baeb2` |
| retrorepair/xemu | `groovy-upstream-sync` | `c5d840daa` |
| retrorepair/flycast-dojo | `groovy-rio-buffer-lifetime` | `c07861d6b` |
| retrorepair/rpcs3 | `groovy-upstream-sync` | `b98401e09` |

Working trees are clean apart from two things that should stay uncommitted: an untracked
`dependency_cache/` in rpcs3, and a moved `core/deps/breakpad` submodule pointer in
flycast-dojo.

## Open — do this first

**The MiSTer needs a reboot.** `/media/fat/MiSTer_phantom` is current (md5
`16eb5f1c2433e09e276570056a2e4c1d`), but the running process is PID 3553 from 2026-10-06
20:02 and has not picked it up. `MiSTer.ini` has `main=MiSTer_phantom`, so that binary *is*
the main MiSTer process, and `/etc/inittab` starts it with `::sysinit:`, not `respawn` —
a core reload does not restart it. Reboot, or load another core and come back. Until then
the GC/WII and PC tabs are on the device but not on screen.

## What landed

### Dolphin — new GroovyNLC integration
`Source/Core/VideoCommon/GroovyMiSTer/`, the PCSX2 engine ported to Dolphin's GPU and
config: video thread scales/reads back/packs, sender thread owns the socket. Verified on
hardware with Super Mario Galaxy at the console's native 640x456 — not the internal
resolution — and confirmed by eye on the CRT.

- Audio two ways: a tap in `Mixer::Mix` mirrors any backend's output, and a new **MiSTer**
  sound backend drains the mixer itself for no host device at all.
- Settings → **MiSTer** pane, all `Config*` widgets bound straight to `Config::Info`, so
  the pane holds no state and needs no save step.
- `arcade_15` by default. On the tri-sync default a 480p title arrived as 31 kHz
  progressive, which is right for a PC monitor and useless for the CRT.

Needs VS 2026 Build Tools (`...\Microsoft Visual Studio\18\BuildTools\...\vcvars64.bat`),
installed this session — see `memory/dolphin-needs-vs2026.md`.

### 86Box — two real bugs behind the "480p" complaint
1. **Interlace was a lie.** A whole progressive frame was being announced as interlace
   mode 1, *fields from client*, so the core read half the lines it was sent. It sends
   mode 2 now and the core splits the frame. The blit byte budget follows the wire value
   for the same reason.
2. **The blit hook throttled emulation.** `video_blit_memtoscreen_monitor` calls
   `video_wait_for_blit_monitor` on the *emulation* thread, so encode + raster-chase was
   charged to the emulated machine — 70 Hz VGA fell to 14 Hz, and because the refresh was
   measured by counting those calls it fed back into the modeline and spiralled. There is
   a sender thread now, and the refresh comes from `mon_actualrenderedframes`, which is
   latched per *emulated* second.

Audio is wired (it existed but nothing called it), monitor preset and interlacing are
settings, and there is a **MiSTer** page in Tools → Settings.

### Launcher
- GC/WII and PC tabs; the strip **scrolls** now, anchored on the selection, so the tab
  count can keep growing. Verified by rendering off-target at a narrow width.
- **Launching from the PC window never worked, for any system** — both the button and the
  double-click rebuilt an id by stripping `[system] ` off the display text, which has no
  `<key>_` prefix. Real ids are kept beside the rows now.
- 86Box VMs are named after their folder (every one stemmed to `86box` and collided),
  container extensions are stripped (`...Sunshine.nkit` → `...Sunshine`), and labels no
  longer wrap into the row beneath.
- Xbox discs read their own `default.xbe` certificate: `eps-h2u.iso` → **Halo 2**,
  Microsoft, 2004.

## Next

- **GameCube/Wii disc titles.** Same treatment as Xbox — the disc header at 0x20 carries
  the real title, so Galaxy and Sunshine could drop their `(Europe, Australia) (En,Fr,...)`
  filename baggage. The user has been offered this and not yet answered.
- **Unify the vendored Groovy client.** The copies have diverged: PCSX2's has
  `gmw_set_keepalive` and a `rioServiceQueues()` patch that drains the RIO send completion
  queue outside `WaitSync` — without it a host that paces itself fills the send CQ and
  `RIOSend` starts failing silently, which is the shape of the halts. 86Box's copy lacks
  both; 86Box's alone carries an `<algorithm>`/`<limits>` include fix it needs under
  MinGW. See `memory/groovy-emulator-forks.md`.
- **Jumbo frames** still untested: core side is ready, needs the OSD option on, client
  MTU 3800 and a jumbo-capable path.
- **Sunshine needs one click** — it is an NKit image and Dolphin's warning dialog blocks
  batch mode. Tick "Don't show this again", or set `SkipNKitWarning = True` under
  `[Interface]`.
- 86Box has no keepalive. Harmless today: it does not advertise `GM_CAP_KEEPALIVE`, so the
  core applies no idle timeout. It would need the newer client first.

## Where things live

Forks under `C:\Users\joelw\Documents\groovy-forks\` except rpcs3
(`C:\Users\joelw\Documents\rpcs3-groovy`), deployed to `C:\Emulators\<name>\`. MiSTer is
`192.168.1.204`; its SSH host key is not in PuTTY's cache, so pass
`-hostkey SHA256:FqNJOsj3FLUoMQxgn+cqGoXvVfENmVK4QFoSCMKl2lU` to plink/pscp or it hangs
waiting for a confirmation it cannot read from stdin.

The launcher the user actually runs is `build_output/PhantomArcadeManager.exe`, not the one
built in `host/`. Its startup ROM scan takes ~85 s before the game list fills.
