#!/usr/bin/env python3
"""mister_inject.py - synthesise keypresses on the MiSTer via /dev/uinput.

Run on the DE10-Nano. Creates a virtual keyboard, presses the keys you name, and
removes it again. Together with mister_fbdump.py this drives and observes the launcher
with nothing plugged in - navigate, screenshot, confirm.

  python3 mister_inject.py down down down          # three presses
  python3 mister_inject.py right enter --settle 1.0

Key names: up down left right enter space esc tab f5.

The device has to linger a moment after creation: MiSTer rescans /dev/input on hotplug
and will miss keys sent before it has opened the new node.
"""
import fcntl
import os
import struct
import sys
import time

UINPUT_IOCTL_BASE = ord('U')


def _iow(nr, size):
    # _IOW(type, nr, size): dir=1 (write), 14-bit size, 8-bit type, 8-bit nr
    return (1 << 30) | (size << 16) | (UINPUT_IOCTL_BASE << 8) | nr


UI_SET_EVBIT = _iow(100, 4)
UI_SET_KEYBIT = _iow(101, 4)
UI_DEV_CREATE = (UINPUT_IOCTL_BASE << 8) | 1
UI_DEV_DESTROY = (UINPUT_IOCTL_BASE << 8) | 2

EV_SYN, EV_KEY = 0x00, 0x01
SYN_REPORT = 0

KEYS = {
    "esc": 1, "space": 57, "enter": 28, "tab": 15, "backspace": 14,
    "up": 103, "left": 105, "right": 106, "down": 108,
    "f1": 59, "f5": 63,
    "w": 17, "a": 30, "s": 31, "d": 32,
}

# input_event on 32-bit ARM: struct timeval (two 32-bit longs) + type + code + value
EVENT_FMT = "@llHHi"
EVENT_SIZE = struct.calcsize(EVENT_FMT)


def emit(fd, etype, code, value):
    t = time.time()
    os.write(fd, struct.pack(EVENT_FMT, int(t), int((t % 1) * 1e6), etype, code, value))


def main():
    names = [a for a in sys.argv[1:] if not a.startswith("--")]
    settle = 0.35
    hold = 0.06
    args = sys.argv[1:]
    for i, a in enumerate(args):
        if a == "--settle" and i + 1 < len(args):
            settle = float(args[i + 1])
        if a == "--hold" and i + 1 < len(args):
            hold = float(args[i + 1])

    unknown = [n for n in names if n not in KEYS]
    if not names or unknown:
        print("usage: mister_inject.py <key>... [--settle S] [--hold S]")
        print("keys:", " ".join(sorted(KEYS)))
        if unknown:
            print("unknown:", " ".join(unknown))
        return 2

    fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
    try:
        fcntl.ioctl(fd, UI_SET_EVBIT, EV_KEY)
        for code in KEYS.values():
            fcntl.ioctl(fd, UI_SET_KEYBIT, code)

        # struct uinput_user_dev: name[80], input_id{bustype,vendor,product,version},
        # ff_effects_max, absmax/absmin/absfuzz/absflat[64] each
        name = b"phantom-inject"
        dev = name.ljust(80, b"\0")
        dev += struct.pack("@HHHH", 0x03, 0x1209, 0x0001, 1)  # BUS_USB
        dev += struct.pack("@i", 0)
        dev += b"\0" * (4 * 64 * 4)
        os.write(fd, dev)
        fcntl.ioctl(fd, UI_DEV_CREATE)

        # let MiSTer notice the hotplug and open the node
        time.sleep(1.2)

        for n in names:
            code = KEYS[n]
            emit(fd, EV_KEY, code, 1)
            emit(fd, EV_SYN, SYN_REPORT, 0)
            time.sleep(hold)
            emit(fd, EV_KEY, code, 0)
            emit(fd, EV_SYN, SYN_REPORT, 0)
            time.sleep(settle)
            print("pressed", n, flush=True)

        time.sleep(0.3)
        fcntl.ioctl(fd, UI_DEV_DESTROY)
    finally:
        os.close(fd)
    return 0


if __name__ == "__main__":
    sys.exit(main())
