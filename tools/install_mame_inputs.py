#!/usr/bin/env python3
"""install_mame_inputs.py - put the Phantom Arcade pad mapping into GroovyMAME.

  python3 tools/install_mame_inputs.py "C:/Emulators/GroovyMAME"   [--revert]

Merges host/mame/phantom-input.xml into <MAME>/cfg/default.cfg. That file is MAME's
own global config and also carries mixer and UI settings, so this inserts (or replaces)
just the <input> element and leaves everything else alone. The original is copied to
default.cfg.phantom-backup the first time, and --revert puts it back.

Why this is needed at all: with -joystickprovider mister the MiSTer's pads arrive as two
joysticks, but MAME still has Coin on keyboard 5 and Start on keyboard 1, so a cabinet
with no keyboard cannot insert a credit. See the comments in phantom-input.xml for what
the provider actually exposes and how the button positions were established.
"""
import os
import re
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FRAGMENT = os.path.join(HERE, "..", "host", "mame", "phantom-input.xml")


def strip_comments(xml):
    return re.sub(r"<!--.*?-->", "", xml, flags=re.S)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    revert = "--revert" in sys.argv
    if not args:
        print(__doc__)
        return 2

    mame_dir = args[0]
    cfg_dir = os.path.join(mame_dir, "cfg")
    cfg = os.path.join(cfg_dir, "default.cfg")
    backup = cfg + ".phantom-backup"

    if revert:
        if os.path.exists(backup):
            shutil.copyfile(backup, cfg)
            print("restored", cfg)
            return 0
        print("no backup at", backup)
        return 1

    if not os.path.isdir(mame_dir):
        print("not a directory:", mame_dir)
        return 1
    os.makedirs(cfg_dir, exist_ok=True)

    with open(FRAGMENT, "r", encoding="utf-8") as f:
        block = strip_comments(f.read()).strip()

    if os.path.exists(cfg):
        if not os.path.exists(backup):
            shutil.copyfile(cfg, backup)
            print("backed up ->", backup)
        with open(cfg, "r", encoding="utf-8-sig") as f:
            doc = f.read()
    else:
        doc = ('<?xml version="1.0"?>\n'
               '<mameconfig version="10">\n'
               '    <system name="default">\n'
               '    </system>\n'
               '</mameconfig>\n')

    # drop any <input> we or the user put there before, so re-running is idempotent
    doc = re.sub(r"[ \t]*<input>.*?</input>\s*", "", doc, flags=re.S)

    indented = "\n".join(("        " + l) if l.strip() else l for l in block.split("\n"))

    # insert before the close of <system name="default">
    m = re.search(r'(<system name="default">)(.*?)(\n\s*</system>)', doc, flags=re.S)
    if not m:
        print("could not find <system name=\"default\"> in", cfg)
        return 1
    doc = doc[:m.end(2)] + "\n" + indented + doc[m.end(2):]

    with open(cfg, "w", encoding="utf-8") as f:
        f.write(doc)

    ports = len(re.findall(r"<port ", block))
    print("installed %d input mappings into %s" % (ports, cfg))
    print("Select = Coin, Start = Start, d-pad on the hat; keyboard defaults kept.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
