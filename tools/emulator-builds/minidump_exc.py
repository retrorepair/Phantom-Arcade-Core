"""minidump_exc.py DUMP - exception code/address from a Windows minidump, and which module the address is in."""
import struct, sys

d = open(sys.argv[1], "rb").read()
sig, ver, nstreams, dir_rva = struct.unpack_from("<4sIII", d, 0)
assert sig == b"MDMP", "not a minidump"
streams = {}
for i in range(nstreams):
    t, size, rva = struct.unpack_from("<III", d, dir_rva + i * 12)
    streams[t] = (size, rva)

def ustr(rva):
    n = struct.unpack_from("<I", d, rva)[0]
    return d[rva + 4: rva + 4 + n].decode("utf-16le")

mods = []
if 4 in streams:                      # ModuleListStream
    size, rva = streams[4]
    n = struct.unpack_from("<I", d, rva)[0]
    for i in range(n):
        base, msize, _, _, name_rva = struct.unpack_from("<QIIII", d, rva + 4 + i * 108)
        mods.append((base, msize, ustr(name_rva)))

if 6 in streams:                      # ExceptionStream
    size, rva = streams[6]
    tid = struct.unpack_from("<I", d, rva)[0]
    code, flags, rec, addr, nparams = struct.unpack_from("<IIQQI", d, rva + 8)
    params = struct.unpack_from("<15Q", d, rva + 8 + 4 + 4 + 8 + 8 + 4 + 4)
    print(f"thread {tid}  exception 0x{code:08X}  at 0x{addr:X}  params {[hex(p) for p in params[:nparams]]}")
    for base, msize, name in mods:
        if base <= addr < base + msize:
            print(f"  in {name}  base 0x{base:X}  offset 0x{addr - base:X}")
            break
    else:
        print("  address is in no loaded module (JIT / freed memory)")
    xemu = [m for m in mods if m[2].lower().endswith("xemu.exe")]
    if xemu:
        print(f"  xemu.exe base 0x{xemu[0][0]:X}")
else:
    print("no exception stream")
print("modules:", ", ".join(sorted({m[2].split(chr(92))[-1] for m in mods if "nv" in m[2].lower() or "amd" in m[2].lower() or "xemu" in m[2].lower()}))[:300])
