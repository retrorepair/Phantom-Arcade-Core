# build_output

What a Phantom Arcade install needs:

| File | Goes to | What it is |
|---|---|---|
| `Phantom_Arcade.rbf` | `/media/fat/_Utility/` | the FPGA bitstream |
| `MiSTer_phantom` | `/media/fat/` (chmod +x) | the core's ARM program, launcher included |
| `PhantomArcadeManager.exe` | the PC | the host daemon that starts emulators |

Plus one line in `/media/fat/MiSTer.ini` — see the top-level [README](../README.md).

## The bitstream is unmodified

`Phantom_Arcade.rbf` is a byte-for-byte copy of upstream's `GroovyNLC_20260904.rbf`:

```
5f5082ed128d024cea9635cb1cfbca44e0280b4016100dff16d9f6ddb708231c  GroovyNLC_20260904.rbf
5f5082ed128d024cea9635cb1cfbca44e0280b4016100dff16d9f6ddb708231c  Phantom_Arcade.rbf
```

Both names are kept so that is checkable. The launcher is entirely in the ARM binary, so
no Quartus rebuild is involved in this project and an existing Groovy user can try it by
replacing one file.

The core name inside the bitstream is still `GroovyNLC`, which is why the `MiSTer.ini`
section is `[GroovyNLC]` whatever you call the `.rbf`.

## The rest

`MiSTer_groovyNLC` is upstream's own ARM binary, kept for comparison and as a way back to
stock behaviour. It does not contain the launcher.
