"""
Pre-upload guard: refuse to flash an image built for the wrong ESP32-P4
silicon major revision.

Chip rev 3.x (ESP32-P4X-Function-EV-Board v1.6) is a breaking major
revision of the P4: a rev-1.x image will not boot on it and vice versa,
and a mismatched flash leaves the board unbootable until reflashed. Each
PlatformIO env declares its target silicon via `custom_p4_rev_major`;
before flashing we ask the connected chip (esptool ROM download mode)
what it actually is and abort on mismatch.
"""

import re
import subprocess
import sys

Import("env")


def _detect_port(env):
    port = env.subst("$UPLOAD_PORT")
    if port:
        return port
    try:
        env.AutodetectUploadPort()
    except Exception:
        pass
    return env.subst("$UPLOAD_PORT")


def _check_chip_rev(source, target, env):
    expected = str(env.GetProjectOption("custom_p4_rev_major", "")).strip()
    if not expected:
        return

    port = _detect_port(env)
    if not port:
        print("check_chip_rev: no serial port detected — skipping revision check")
        return

    cmd = [
        env.subst("$PYTHONEXE"), "-m", "esptool",
        "--port", port, "--no-stub", "read-mac",
    ]
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
        out = proc.stdout + proc.stderr
    except Exception as exc:  # esptool missing, port busy, timeout…
        print("check_chip_rev: could not query chip (%s) — skipping check" % exc)
        return

    match = re.search(r"revision v(\d+)\.(\d+)", out)
    if not match:
        print("check_chip_rev: no chip revision in esptool output — skipping check")
        return

    major, minor = match.group(1), match.group(2)
    if major == expected:
        print("check_chip_rev: chip rev v%s.%s matches env target v%s.x — OK"
              % (major, minor, expected))
        return

    sys.stderr.write(
        "\n*** UPLOAD BLOCKED by tools/check_chip_rev.py ***\n"
        "Connected chip is ESP32-P4 silicon rev v%s.%s, but env '%s' builds\n"
        "for rev v%s.x. Flashing this image would leave the board unbootable.\n"
        "Use the matching env instead:\n"
        "    rev v1.x board (EV-Board v1.5.2):  pio run -e esp32-p4-evboard -t upload\n"
        "    rev v3.x board (P4X EV-Board v1.6): pio run -e esp32-p4x-evboard -t upload\n\n"
        % (major, minor, env["PIOENV"], expected)
    )
    env.Exit(1)


env.AddPreAction("upload", _check_chip_rev)
