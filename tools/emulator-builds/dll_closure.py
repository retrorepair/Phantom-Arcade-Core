"""dll_closure.py EXE DESTDIR SEARCHDIR... - copy every non-system DLL EXE needs, transitively, next to it.

get_deps.py only follows the exe's direct imports, and only inside /mingw64/bin. xemu.exe imports libcurl, whose own
imports (nghttp3, ngtcp2, ...) are what Windows reported missing, and libslirp-0.dll, which meson builds into the
build tree. Reads imports with objdump -p, resolves each against SEARCHDIRs, and stops when nothing new turns up.
A DLL found nowhere is reported unless it is a Windows system DLL.
"""
import os, re, shutil, subprocess, sys

exe, dest, *search = sys.argv[1:]
SYSTEM = {d.lower() for d in os.listdir(r"C:\Windows\System32") if d.lower().endswith(".dll")}

def imports(path):
    out = subprocess.run(["objdump", "-p", path], capture_output=True, text=True).stdout
    return {m.lower() for m in re.findall(r"DLL Name:\s*(\S+)", out)}

index = {}
for d in search:
    for dp, _, fns in os.walk(d):
        for fn in fns:
            if fn.lower().endswith(".dll"):
                index.setdefault(fn.lower(), os.path.join(dp, fn))

have = {f.lower() for f in os.listdir(dest) if f.lower().endswith(".dll")}
todo = [exe] + [os.path.join(dest, f) for f in os.listdir(dest) if f.lower().endswith(".dll")]
seen = set()
missing = set()
while todo:
    p = todo.pop()
    if p in seen:
        continue
    seen.add(p)
    for dll in imports(p):
        if dll in have or dll.startswith("api-ms-win-") or dll.startswith("ext-ms-win-") or dll in SYSTEM:
            continue
        src = index.get(dll)
        if not src:
            missing.add(dll)
            continue
        shutil.copy2(src, os.path.join(dest, os.path.basename(src)))
        have.add(dll)
        todo.append(os.path.join(dest, os.path.basename(src)))
        print("copied", dll, "from", src)
print("still missing:", sorted(missing) or "none")
