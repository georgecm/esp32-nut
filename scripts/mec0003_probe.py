#!/usr/bin/env python3
"""Probe what a UPS behind a MEC0003 USB bridge (USB ID 0001:0000) really does.

Tescom's Leo LCD, Leo SmartBox, Neoline and LiON all talk Megatec/Q1 through this
cheap bridge. The bridge does not take Megatec text: each command is a USB string
descriptor request whose *index* stands for a command, and NUT knows two different
index maps for it (nutdrv_qx.c):

    command                    hunnox (Leo, SmartBox, Neoline)   krauler (LiON RT)
    Q1 status / F / I          3 / 13 / 12                        3 / 13 / 12
    Q  beeper toggle           7                                  7
    C  cancel shutdown/load on 10 (0x0a)                          11 (0x0b)
    T  10 s battery test       -                                  4
    TL deep battery test       -                                  5
    S.. shutdowns              16.. (computed, see below)         -

So a command that "fails" through NUT may only be missing from the map NUT used.
This tool sends specific indexes one at a time and watches the Q1 status bits and
the output voltage, so each model gets a measured command list instead of a guess.

It never sweeps unknown indexes: the shutdown commands are indexes too, so a blind
scan could switch the output off. Every test that can affect power asks first.

Usage (the UPS's NUT driver must not hold the bridge; --stop-nut pauses and restores it):

    brew install libusb && python3 -m venv .venv-usb && .venv-usb/bin/pip install pyusb
    .venv-usb/bin/python scripts/mec0003_probe.py --model "Leo SmartBox" --stop-nut
    .venv-usb/bin/python scripts/mec0003_probe.py --list            # the tests and their risk
    .venv-usb/bin/python scripts/mec0003_probe.py --only beeper_hunnox,test_quick_krauler

Risk levels: read (nothing changes), low (beeper; UPS on battery for ~10 s), medium
(a 30 s shutdown countdown that the test then tries to cancel: if cancelling fails the
output switches off), high (output off and back, deep battery test). By default it runs
read and low and asks before each medium/high test; --max-risk limits it.

Writes a JSON report (device strings, ratings, each test's status timeline and verdict)
and prints suggested `# Commands:` lines for nut-app's profiles/<model>.conf.
"""

import argparse
import json
import os
import platform
import subprocess
import sys
import time
from datetime import datetime

VID, PID = 0x0001, 0x0000
LANGID = 0x0409
LENGTH = 102  # UPSmart's length; MEC0003 bridges only refresh their reply for 102-byte requests
RISKS = ["read", "low", "medium", "high"]

# hunnox shutdown index (nutdrv_qx.c hunnox_command): delay .5 min -> 0x10 stay off, +8 -> return
HUNNOX_STAYOFF_30S = 0x10
HUNNOX_RETURN_30S = 0x18

BITS = ["utility_fail", "battery_low", "bypass_boost", "ups_failed",
        "standby_type", "test_in_progress", "shutdown_active", "beeper_on"]


# ---------------------------------------------------------------- USB

def open_bridge():
    try:
        import usb.core
        import usb.backend.libusb1 as libusb1
    except ImportError:
        sys.exit("pyusb missing: python3 -m venv .venv-usb && .venv-usb/bin/pip install pyusb")
    backend = None
    for lib in ("/opt/homebrew/lib/libusb-1.0.dylib", "/usr/local/lib/libusb-1.0.dylib"):
        if os.path.exists(lib):
            backend = libusb1.get_backend(find_library=lambda _x, lib=lib: lib)
            break
    dev = usb.core.find(idVendor=VID, idProduct=PID, backend=backend)
    if dev is None:
        sys.exit("No MEC0003 bridge (0001:0000) found. Is the UPS's USB cable in this computer?")
    return dev


def get_string(dev, index, length=LENGTH):
    """Raw string descriptor `index`, decoded from UTF-16LE; None when the bridge doesn't answer."""
    try:
        data = bytes(dev.ctrl_transfer(0x80, 0x06, (0x03 << 8) | index, LANGID, length, timeout=1500))
    except Exception:
        return None
    if len(data) < 2:
        return None
    text = data[2:data[0]].decode("utf-16-le", errors="replace")
    return text.split("\r")[0]


def send(dev, index):
    """A command is just a string request; its reply is usually empty or junk."""
    return get_string(dev, index)


# ---------------------------------------------------------------- Q1

def parse_q1(raw):
    """'(MMM.M NNN.N PPP.P QQQ RR.R S.SS TT.T b7b6b5b4b3b2b1b0' -> dict, or None.
    The bridge often puts junk in the first byte (krauler replaces it with '(')."""
    if not raw or len(raw) < 40:
        return None
    parts = ("(" + raw[1:]).lstrip("(").split()
    if len(parts) < 8 or len(parts[7]) != 8 or set(parts[7]) - {"0", "1"}:
        return None
    try:
        q = {"input_v": float(parts[0]), "fault_v": float(parts[1]), "output_v": float(parts[2]),
             "load": int(parts[3]), "freq": float(parts[4]), "battery_v": float(parts[5]),
             "temp": float(parts[6])}
    except ValueError:
        return None
    for name, bit in zip(BITS, parts[7]):
        q[name] = bit == "1"
    return q


def status(dev):
    for _ in range(3):
        q = parse_q1(get_string(dev, 0x03))
        if q:
            return q
        time.sleep(0.3)
    return None


def short(q):
    if not q:
        return "no reply"
    flags = [n for n in BITS if q.get(n)]
    return f"in {q['input_v']:.0f} V out {q['output_v']:.0f} V load {q['load']}% batt {q['battery_v']:.2f} V {' '.join(flags) or '-'}"


def watch(dev, seconds, every=1.0, label=""):
    timeline, start = [], time.time()
    while time.time() - start < seconds:
        q = status(dev)
        t = round(time.time() - start, 1)
        timeline.append({"t": t, **(q or {"no_reply": True})})
        print(f"   {label}+{t:5.1f}s  {short(q)}")
        time.sleep(every)
    return timeline


def any_true(timeline, key):
    return any(s.get(key) for s in timeline)


def all_powered(timeline):
    return all(s.get("output_v", 0) > 50 for s in timeline if "output_v" in s)


# ---------------------------------------------------------------- tests

def t_beeper(dev, index):
    before = status(dev)
    send(dev, index)
    tl1 = watch(dev, 3, label="after 1st ")
    send(dev, index)  # toggle back
    tl2 = watch(dev, 3, label="after 2nd ")
    changed = any(s.get("beeper_on") != (before or {}).get("beeper_on") for s in tl1 if "beeper_on" in s)
    return {"verdict": "works (beeper bit follows)" if changed else "no visible change (bit only shows while sounding, or ignored)",
            "works": True if changed else None, "timeline": tl1 + tl2}


def t_battery_test(dev, index, seconds=30):
    send(dev, index)
    tl = watch(dev, seconds)
    started = any_true(tl, "test_in_progress") or any_true(tl, "utility_fail")
    return {"verdict": "works (test bit / on battery seen)" if started else "no test seen",
            "works": started, "timeline": tl}


def t_cancel(dev, cancel_index):
    """Start a 30 s stay-off countdown (hunnox index), then try to cancel it."""
    send(dev, HUNNOX_STAYOFF_30S)
    tl0 = watch(dev, 4, label="countdown ")
    if not any_true(tl0, "shutdown_active"):
        return {"verdict": "countdown did not start, nothing to cancel", "works": None, "timeline": tl0}
    send(dev, cancel_index)
    tl1 = watch(dev, 40, every=2, label="after cancel ")
    ok = all_powered(tl1) and not tl1[-1].get("shutdown_active")
    return {"verdict": "works (countdown cancelled, output stayed on)" if ok else
            "did not cancel (output switched off: press the UPS's button or unplug/replug it at the wall)",
            "works": ok, "timeline": tl0 + tl1}


def t_restart(dev):
    send(dev, HUNNOX_RETURN_30S)
    tl = watch(dev, 120, every=3)
    went_off = any(s.get("output_v", 999) < 50 for s in tl)
    came_back = went_off and tl[-1].get("output_v", 0) > 50
    return {"verdict": "works (off, then back by itself)" if came_back else
            ("off but did not come back within 2 minutes" if went_off else "did not switch off"),
            "works": came_back, "timeline": tl}


TESTS = [
    # name, NUT command it stands for, map, risk, description, function
    ("beeper_hunnox", "beeper.toggle", "hunnox/krauler 0x07", "low",
     "toggle the beeper twice (back to how it was)", lambda d: t_beeper(d, 0x07)),
    ("test_quick_krauler", "test.battery.start.quick", "krauler 0x04", "low",
     "10 s battery test: the UPS runs on its battery for ~10 s", lambda d: t_battery_test(d, 0x04)),
    ("cancel_hunnox", "shutdown.stop / load.on", "hunnox 0x0a", "medium",
     "start a 30 s off countdown, then send hunnox cancel", lambda d: t_cancel(d, 0x0a)),
    ("cancel_krauler", "shutdown.stop / load.on", "krauler 0x0b", "medium",
     "start a 30 s off countdown, then send krauler cancel", lambda d: t_cancel(d, 0x0b)),
    ("restart_hunnox", "shutdown.return", "hunnox 0x18", "high",
     "output off after 30 s; should come back by itself", t_restart),
    ("test_deep_krauler", "test.battery.start.deep", "krauler 0x05", "high",
     "deep battery test: runs on battery until low (cancelled after 20 s)",
     lambda d: (lambda r: (send(d, 0x0b), r)[1])(t_battery_test(d, 0x05, 20))),
]


# ---------------------------------------------------------------- NUT driver

def nut_driver(action):
    """Pause or restore the NUT driver that holds the bridge (Tescom Connect on macOS, systemd on Linux)."""
    if platform.system() == "Darwin":
        plist = "/Library/LaunchDaemons/gr.tescom.upsmonitor.nut-driver.plist"
        cmd = (["sudo", "launchctl", "bootout", "system/gr.tescom.upsmonitor.nut-driver"] if action == "stop"
               else ["sudo", "launchctl", "bootstrap", "system", plist])
    else:
        cmd = ["sudo", "systemctl", action, "nut-driver.target"]
    print(f"== {'Pausing' if action == 'stop' else 'Restoring'} the NUT driver: {' '.join(cmd)}")
    subprocess.run(cmd, check=False)
    time.sleep(3)


# ---------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--model", default="unknown", help="model name for the report, e.g. 'Leo SmartBox'")
    ap.add_argument("--max-risk", choices=RISKS, default="high", help="skip tests above this risk")
    ap.add_argument("--only", help="comma-separated test names")
    ap.add_argument("--yes", action="store_true", help="don't ask before medium/high tests")
    ap.add_argument("--stop-nut", action="store_true", help="pause the NUT driver during the probe (needs sudo)")
    ap.add_argument("--list", action="store_true", help="list the tests and exit")
    ap.add_argument("--out", help="report path (default ups-probe-<model>-<time>.json)")
    a = ap.parse_args()

    if a.list:
        for name, nut, imap, risk, desc, _ in TESTS:
            print(f"{name:20} {risk:7} {nut:26} {imap:22} {desc}")
        return

    if a.stop_nut:
        nut_driver("stop")
    report = {"model": a.model, "usb_id": "0001:0000", "time": datetime.now().isoformat(timespec="seconds"),
              "host": platform.node(), "tests": {}}
    try:
        dev = open_bridge()
        print("== Waking the bridge (102-byte Q1 requests, as UPSmart does)")
        for _ in range(3):
            get_string(dev, 0x03)
            time.sleep(0.5)
        report["strings"] = {"manufacturer": get_string(dev, 1, 255), "product": get_string(dev, 2, 255)}
        report["ratings_F"] = get_string(dev, 0x0d)
        report["info_I"] = get_string(dev, 0x0c)
        q = status(dev)
        report["status_Q1"] = q
        print(f"   strings {report['strings']}\n   F {report['ratings_F']!r}\n   I {report['info_I']!r}\n   Q1 {short(q)}")
        if not q:
            sys.exit("No Q1 status from the bridge: is a NUT driver still holding it? (use --stop-nut)")
        if q["utility_fail"]:
            sys.exit("The UPS is on battery right now; plug it into mains before probing.")

        only = set(a.only.split(",")) if a.only else None
        for name, nut, imap, risk, desc, fn in TESTS:
            if only and name not in only:
                continue
            if RISKS.index(risk) > RISKS.index(a.max_risk):
                report["tests"][name] = {"skipped": f"risk {risk} above --max-risk"}
                continue
            print(f"\n== {name} [{risk}] {desc}")
            if risk in ("medium", "high") and not a.yes:
                if input("   Run it? Nothing important should be plugged into the UPS. [y/N] ").strip().lower() != "y":
                    report["tests"][name] = {"skipped": "declined"}
                    continue
            result = fn(dev)
            result.update({"nut_command": nut, "index_map": imap, "risk": risk})
            report["tests"][name] = result
            print(f"   -> {result['verdict']}")
            after = status(dev)
            if after and after["output_v"] < 50:
                input("   The output is off. Press the UPS's power button (or unplug/replug it at the wall), then Enter... ")
            time.sleep(3)
    finally:
        if a.stop_nut:
            nut_driver("start")

    out = a.out or f"ups-probe-{a.model.replace(' ', '-').lower()}-{datetime.now():%Y%m%d-%H%M}.json"
    with open(out, "w") as f:
        json.dump(report, f, indent=2)
    print(f"\n== Report: {out}\n")
    works = [r["nut_command"] for r in report["tests"].values() if r.get("works") is True]
    fails = [r["nut_command"] for r in report["tests"].values() if r.get("works") is False]
    print("Suggested profile lines (check before copying into nut-app profiles/<model>.conf):")
    print(f"# Commands: {', '.join(dict.fromkeys(works)) or '(none verified)'}")
    if fails:
        print(f"# Commands not working: {', '.join(dict.fromkeys(fails))}")
    for name, r in report["tests"].items():
        print(f"  {name:20} {r.get('verdict') or r.get('skipped')}")


if __name__ == "__main__":
    main()
