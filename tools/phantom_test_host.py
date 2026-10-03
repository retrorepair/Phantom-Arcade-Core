#!/usr/bin/env python3
"""phantom_test_host.py - a stand-in for the Phantom Arcade PC daemon.

Speaks the same UDP protocol as PhantomArcadeManager, so the core's launcher can be
exercised end to end without a Windows box, GroovyMAME, or any ROMs. It answers
discovery, serves a catalog, and logs what it is asked to launch - it does not start
anything, which is the point: it isolates the core's half of the protocol.

  python3 phantom_test_host.py [--port 1999] [--catalog file.json] [--quiet]

Run it on any machine the MiSTer can reach, including the MiSTer itself (with the
core's PC_SERVER_IP set to 127.0.0.1), which avoids desktop firewall rules entirely
when all you want to test is the core.
"""
import argparse
import json
import socket
import sys
import time

DEFAULT_CATALOG = {
    "games": [
        {"id": "groovymame_kinst",  "title": "Killer Instinct (v1.5, USA)",
         "system": "groovymame", "systemName": "Midway", "videoMode": "240p @ 60.0Hz"},
        {"id": "groovymame_sfiii3", "title": "Street Fighter III: 3rd Strike (Euro 990512)",
         "system": "groovymame", "systemName": "Capcom CPS-3", "videoMode": "224p @ 59.6Hz"},
        {"id": "groovymame_garou",  "title": "Garou: Mark of the Wolves",
         "system": "groovymame", "systemName": "SNK Neo-Geo", "videoMode": "224p @ 59.2Hz"},
        {"id": "groovymame_mslug3", "title": "Metal Slug 3",
         "system": "groovymame", "systemName": "SNK Neo-Geo", "videoMode": "224p @ 59.2Hz"},
        {"id": "groovymame_umk3",   "title": "Ultimate Mortal Kombat 3",
         "system": "groovymame", "systemName": "Midway Wolf", "videoMode": "254p @ 54.7Hz"},
        {"id": "groovymame_dkong",  "title": "Donkey Kong",
         "system": "groovymame", "systemName": "Nintendo", "videoMode": "224p @ 60.6Hz"},
        {"id": "flycast_cvs2",      "title": "Capcom vs. SNK 2",
         "system": "flycast", "systemName": "Sega NAOMI", "videoMode": "240p @ 60.0Hz"},
        {"id": "flycast_ikaruga",   "title": "Ikaruga",
         "system": "flycast", "systemName": "Sega NAOMI", "videoMode": "240p Tate"},
        {"id": "flycast_vf4ft",     "title": "Virtua Fighter 4 Final Tuned",
         "system": "flycast", "systemName": "Sega NAOMI 2", "videoMode": "480i @ 60.0Hz"},
        {"id": "pcsx2_arcana",      "title": "Arcana Heart",
         "system": "pcsx2", "systemName": "PlayStation 2", "videoMode": "240p @ 60.0Hz"},
        {"id": "pcsx2_espgaluda",   "title": "Espgaluda",
         "system": "pcsx2", "systemName": "PlayStation 2", "videoMode": "240p @ 60.0Hz"},
        {"id": "dolphin_melee",     "title": "Super Smash Bros. Melee",
         "system": "dolphin", "systemName": "GameCube", "videoMode": "480i @ 60.0Hz"},
        {"id": "dolphin_fzerogx",   "title": "F-Zero GX",
         "system": "dolphin", "systemName": "GameCube", "videoMode": "480i @ 60.0Hz"},
    ]
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=1999)
    ap.add_argument("--catalog")
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--seconds", type=int, default=0, help="exit after N seconds (0 = run forever)")
    args = ap.parse_args()

    catalog = DEFAULT_CATALOG
    if args.catalog:
        with open(args.catalog) as f:
            catalog = json.load(f)
    blob = json.dumps(catalog).encode()

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    s.bind(("0.0.0.0", args.port))
    s.settimeout(1.0)

    started = time.time()
    n_games = len(catalog.get("games", []))
    print("phantom_test_host on :%d, %d titles, %d bytes of catalog"
          % (args.port, n_games, len(blob)), flush=True)

    while True:
        if args.seconds and time.time() - started > args.seconds:
            print("time limit reached, exiting", flush=True)
            return
        try:
            data, addr = s.recvfrom(4096)
        except socket.timeout:
            continue

        msg = data.decode("utf-8", "replace").strip()
        if not args.quiet:
            print("<- %-22s %s" % ("%s:%d" % addr, msg), flush=True)

        if msg.startswith("DISCOVER_PHANTOM"):
            reply = b"PHANTOM_HOST_ONLINE:%d:8088" % args.port
        elif msg == "GET_CATALOG":
            reply = blob
        elif msg == "PING":
            reply = b"PONG"
        elif msg.startswith("LAUNCH:"):
            gid = msg[7:]
            print("   LAUNCH requested: %s  (test host starts nothing)" % gid, flush=True)
            reply = b"ACK:LAUNCH:OK:" + gid.encode()
        elif msg == "KILL":
            print("   KILL requested", flush=True)
            reply = b"ACK:KILL:OK"
        else:
            continue

        s.sendto(reply, addr)
        if not args.quiet:
            print("-> %-22s %s" % ("%s:%d" % addr, reply[:60].decode("utf-8", "replace")
                                   + ("..." if len(reply) > 60 else "")), flush=True)


if __name__ == "__main__":
    sys.exit(main())
