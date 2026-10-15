#!/usr/bin/env python3
"""Rewrite FPP playlists' TP-Link commands from IP addresses to device names.

The plugin's commands used to take an IP, a plug number and a device type
("192.168.110.158", "true", "0", "tplink"). They now take the device's name
from plugin.tplink.json ("LED Roof", "true"). This rewrites every playlist on
an FPP box to the new form, looking each IP (and plug number) up in the box's
own plugin.tplink.json.

Runs from any machine that can reach FPP's web API. Without --apply it only
prints what it would change. With --apply it first saves every playlist it is
about to change, as FPP returned it, into --backup-dir, then writes the new
ones and reads each back to check it. If any IP has no name in the config,
nothing is written at all.

    tools/migrate_playlists_to_names.py --host 192.168.86.11
    tools/migrate_playlists_to_names.py --host 192.168.86.11 --apply --backup-dir ~/fpp-playlist-backup
"""
from __future__ import annotations

import argparse
import copy
import json
import os
import sys
import urllib.parse
import urllib.request

# Command -> (number of arguments after the device in the NEW form). The old
# form had the same arguments plus the plug number and/or type at the end.
COMMANDS = {
    "TPLink Set Switch": 1,       # state            (old: state, plug, type)
    "TPLink Toggle Switch": 1,    # delay            (old: delay, plug, type)
    "TPLink Set Light RGB": 5,    # r g b temp period (old: + type)
    "TPLink Set Light HSV": 5,    # h s v temp period (old: + type)
    "TPLink Set Light Off": 0,    #                   (old: type)
}
# Commands whose old form carried a plug number right after the kept arguments.
HAS_PLUG = {"TPLink Set Switch", "TPLink Toggle Switch"}


def get_json(host: str, path: str):
    with urllib.request.urlopen("http://%s/%s" % (host, path), timeout=15) as r:
        return json.load(r)


def post_json(host: str, path: str, body) -> None:
    data = json.dumps(body).encode()
    req = urllib.request.Request("http://%s/%s" % (host, path), data=data, method="POST",
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=15) as r:
        r.read()


def name_table(config: list) -> tuple[dict, dict, set]:
    """(ip, plug) -> name, ip -> [names], and the set of names (lower case)."""
    by_ip_plug, by_ip, names = {}, {}, set()
    for e in config:
        ip, name = str(e.get("ip", "")).strip(), str(e.get("name", "")).strip()
        if not ip or not name:
            continue
        by_ip_plug[(ip, int(e.get("plugnumber", 0) or 0))] = name
        by_ip.setdefault(ip, []).append(name)
        names.add(name.lower())
    return by_ip_plug, by_ip, names


def rewrite(item: dict, table, problems: list, where: str) -> bool:
    """Rewrites one command item in place. True when it changed."""
    command = item.get("command")
    if command not in COMMANDS:
        return False
    by_ip_plug, by_ip, names = table
    args = [str(a) for a in item.get("args", [])]
    keep = COMMANDS[command]
    if not args:
        problems.append("%s: %s has no device" % (where, command))
        return False
    target = args[0]
    tokens = [t.strip() for t in target.split(",") if t.strip()]
    if all(t.lower() in names for t in tokens) and len(args) <= 1 + keep:
        return False  # already by name
    plug = 0
    if command in HAS_PLUG and len(args) > 1 + keep:
        try:
            plug = int(args[1 + keep])
        except ValueError:
            plug = 0
    resolved = []
    for t in tokens:
        name = by_ip_plug.get((t, plug))
        if name is None and plug == 0 and len(by_ip.get(t, [])) == 1:
            name = by_ip[t][0]
        if name is None and t.lower() in names:
            name = t
        if name is None:
            problems.append("%s: %s targets %s (plug %d), which has no name in plugin.tplink.json"
                            % (where, command, t, plug))
            return False
        resolved.append(name)
    item["args"] = [", ".join(resolved)] + args[1:1 + keep]
    return True


def walk(node, table, problems: list, where: str, changes: list) -> None:
    if isinstance(node, dict):
        if "command" in node:
            before = list(node.get("args", []))
            if rewrite(node, table, problems, where):
                changes.append((where, node["command"], before, node["args"]))
        for key, value in node.items():
            walk(value, table, problems, where, changes)
    elif isinstance(node, list):
        for value in node:
            walk(value, table, problems, where, changes)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", required=True, help="FPP host or IP")
    ap.add_argument("--apply", action="store_true", help="write the changes (default: only show them)")
    ap.add_argument("--backup-dir", help="where --apply saves the original playlists (required with --apply)")
    a = ap.parse_args()
    if a.apply and not a.backup_dir:
        ap.error("--apply needs --backup-dir")

    table = name_table(get_json(a.host, "api/configfile/plugin.tplink.json"))
    originals, updated, problems, changes = {}, {}, [], []
    for name in get_json(a.host, "api/playlists"):
        pl = get_json(a.host, "api/playlist/" + urllib.parse.quote(name))
        new = copy.deepcopy(pl)
        before = len(changes)
        walk(new, table, problems, name, changes)
        if len(changes) > before:
            originals[name], updated[name] = pl, new

    for where, command, old, new in changes:
        print("%s: %s %s -> %s" % (where, command, old, new))
    print("\n%d command(s) in %d playlist(s) to change." % (len(changes), len(updated)))
    if problems:
        print("\nNOT CHANGING ANYTHING: %d problem(s):" % len(problems))
        for p in problems:
            print("  " + p)
        return 1
    if not a.apply or not updated:
        return 0

    os.makedirs(os.path.expanduser(a.backup_dir), exist_ok=True)
    for name, pl in originals.items():
        path = os.path.join(os.path.expanduser(a.backup_dir), name + ".json")
        with open(path, "w") as f:
            json.dump(pl, f, indent=2)
    print("Backed up %d playlist(s) to %s" % (len(originals), a.backup_dir))

    failed = 0
    for name, pl in updated.items():
        post_json(a.host, "api/playlist/" + urllib.parse.quote(name), pl)
        back = get_json(a.host, "api/playlist/" + urllib.parse.quote(name))
        left = []
        walk(copy.deepcopy(back), table, [], name, left)
        if left:
            failed += 1
            print("  %s: read back still has %d IP command(s)" % (name, len(left)))
        else:
            print("  %s: written and read back by name" % name)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
