# GroovyMiSTer Browser Monitor

This optional monitor mirrors the RGB frame that the PC-side GroovyMiSTer client is already sending to the MiSTer. It does not capture the CRT output, change the modeline, or send input back to the emulator.

## Enable

Set the environment variable `GM_BROWSER_MONITOR=1` in the environment inherited by the emulator/client process, then start the emulator as usual. The HTTP server starts automatically on the first video frame; there is no separate program or third-party dependency to install.

On Windows, if you start the emulator from Command Prompt:

```bat
set GM_BROWSER_MONITOR=1
start "" "C:\\path\\to\\your\\emulator.exe"
```

If Phantom Arcade starts the emulator itself, start Phantom Arcade from that same Command Prompt so the variable is inherited. For a permanent setting, add `GM_BROWSER_MONITOR` with value `1` in Windows Environment Variables, then restart Phantom Arcade/the emulator.

## View

Open `http://<PC-IP>:8765/` from a browser on the same LAN. For this setup, the likely address is `http://192.168.1.126:8765/` if the PC still has that IP. If Windows Firewall asks, allow the existing emulator/client on private networks.

The page polls a read-only endpoint and displays the latest frame. It is intended as a secondary preview; it is not a low-latency interactive display and does not provide browser controls. The CRT path is unchanged.

## Notes

- Disabled by default; without the environment variable, no listener or frame copies are started.
- The server listens on TCP port 8765 on all local interfaces. Use only on a trusted LAN.
- The stream is the frame supplied by the emulator before GroovyMiSTer compression. It is not a capture of MiSTer's physical analog output.
- Build the existing client with this branch's `api/browser_monitor.h` and `api/groovymister.cpp` changes.
