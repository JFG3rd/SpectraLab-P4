#!/usr/bin/env python3
"""
List connected ESP32-P4 / P4X boards and the serial port each is on.

Distinguishes boards by silicon major revision (queried via esptool):
  rev v1.x  -> ESP32-P4-Function-EV-Board v1.5.2   (env: esp32-p4-evboard)
  rev v3.x  -> ESP32-P4X-Function-EV-Board v1.6    (env: esp32-p4x-evboard)

Usage:
  ./tools/list_p4_ports.py
  python3 tools/list_p4_ports.py
  pio run -t list_ports   # if wired as a PlatformIO extra target (optional)
"""

from __future__ import annotations

import glob
import os
import re
import subprocess
import sys
from pathlib import Path


def _find_python_with_esptool() -> str:
    """Prefer PlatformIO's venv python (has esptool); fall back to current."""
    home = Path.home()
    candidates = [
        home / ".platformio" / "penv" / "bin" / "python",
        home / ".platformio" / "penv" / "bin" / "python3",
    ]
    for c in candidates:
        if c.is_file() and os.access(c, os.X_OK):
            return str(c)
    return sys.executable


def _candidate_ports() -> list[str]:
    """Serial callout devices that look like ESP USB-Serial/JTAG or USB-UART."""
    patterns = [
        "/dev/cu.usbmodem*",
        "/dev/cu.usbserial*",
        "/dev/cu.SLAB_USBtoUART*",
        "/dev/cu.wchusbserial*",
        "/dev/cu.ttyUSB*",
    ]
    ports: list[str] = []
    for pat in patterns:
        ports.extend(sorted(glob.glob(pat)))
    # Stable unique order
    seen: set[str] = set()
    out: list[str] = []
    for p in ports:
        if p not in seen:
            seen.add(p)
            out.append(p)
    return out


def _board_label(major: str | None) -> tuple[str, str]:
    """Return (short_name, pio_env) from silicon major revision."""
    if major == "1":
        return ("P4  (EV-Board v1.5.2)", "esp32-p4-evboard")
    if major == "3":
        return ("P4X (EV-Board v1.6)", "esp32-p4x-evboard")
    if major is None:
        return ("unknown", "")
    return (f"P4-family (rev v{major}.x)", "")


def _query_port(python: str, port: str, timeout: float = 45.0) -> dict:
    """Run esptool chip-id / read-mac style probe on one port."""
    cmd = [
        python,
        "-m",
        "esptool",
        "--port",
        port,
        "--no-stub",
        "chip-id",
    ]
    try:
        proc = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=timeout,
        )
        out = (proc.stdout or "") + (proc.stderr or "")
    except subprocess.TimeoutExpired:
        return {"port": port, "ok": False, "error": "timeout talking to device"}
    except FileNotFoundError:
        return {"port": port, "ok": False, "error": f"esptool not found via {python}"}
    except Exception as exc:  # noqa: BLE001 — surface any probe failure cleanly
        return {"port": port, "ok": False, "error": str(exc)}

    # Examples:
    #   Chip type:          ESP32-P4 (revision v3.2)
    #   MAC:                e8:f6:0a:e7:6f:cb
    chip = None
    m_chip = re.search(
        r"Chip type:\s*(\S+(?:\s+\S+)?)\s*\(revision v(\d+)\.(\d+)\)",
        out,
    )
    if not m_chip:
        # Older / alternate wording
        m_chip = re.search(r"Chip is\s+(\S+)\s*\(revision v(\d+)\.(\d+)\)", out)
    rev_major = rev_minor = None
    if m_chip:
        chip = m_chip.group(1).strip()
        rev_major, rev_minor = m_chip.group(2), m_chip.group(3)

    mac = None
    m_mac = re.search(r"MAC:\s*([0-9a-fA-F:]{17})", out)
    if m_mac:
        mac = m_mac.group(1).lower()

    if proc.returncode != 0 and not (chip or mac):
        # Keep a short error snippet for non-ESP devices on usbmodem*
        err = out.strip().splitlines()
        tail = err[-1] if err else f"esptool exit {proc.returncode}"
        return {"port": port, "ok": False, "error": tail}

    label, env = _board_label(rev_major)
    return {
        "port": port,
        "ok": True,
        "chip": chip or "unknown",
        "rev_major": rev_major,
        "rev_minor": rev_minor,
        "revision": f"v{rev_major}.{rev_minor}" if rev_major else "unknown",
        "mac": mac or "unknown",
        "board": label,
        "pio_env": env,
    }


def main() -> int:
    python = _find_python_with_esptool()
    ports = _candidate_ports()

    print("SpectraLab P4 / P4X port map")
    print(f"esptool via: {python}")
    print()

    if not ports:
        print("No USB serial ports found matching cu.usbmodem* / cu.usbserial*.")
        print("Plug in a board and try again.")
        return 1

    # Quick esptool availability check
    probe = subprocess.run(
        [python, "-m", "esptool", "version"],
        capture_output=True,
        text=True,
    )
    if probe.returncode != 0:
        print(
            "error: could not run `python -m esptool`.\n"
            "Install PlatformIO (provides ~/.platformio/penv) or: pip install esptool",
            file=sys.stderr,
        )
        return 2

    results = [_query_port(python, p) for p in ports]

    # Table header
    rows = []
    for r in results:
        if r.get("ok"):
            rows.append(
                (
                    r["board"],
                    r["port"],
                    r["revision"],
                    r["mac"],
                    r.get("pio_env") or "-",
                )
            )
        else:
            rows.append(("not ESP / busy", r["port"], "-", "-", r.get("error", "")[:40]))

    headers = ("Board", "Port", "Chip rev", "MAC", "PIO env")
    widths = [len(h) for h in headers]
    for row in rows:
        for i, cell in enumerate(row):
            widths[i] = max(widths[i], len(str(cell)))

    def fmt(row: tuple) -> str:
        return "  ".join(str(cell).ljust(widths[i]) for i, cell in enumerate(row))

    print(fmt(headers))
    print(fmt(tuple("-" * w for w in widths)))
    for row in rows:
        print(fmt(row))

    ok = [r for r in results if r.get("ok")]
    print()
    if not ok:
        print("No ESP32-P4 devices responded. Is a board in download/serial mode?")
        return 1

    print("Upload examples:")
    for r in ok:
        env = r.get("pio_env")
        if not env:
            continue
        print(f"  pio run -e {env} -t upload --upload-port {r['port']}")
        print(f"  pio device monitor -p {r['port']} -b 115200")

    return 0


if __name__ == "__main__":
    sys.exit(main())
