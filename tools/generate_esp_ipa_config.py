"""
Generate esp_video_ipa_config.c before compilation.

PlatformIO's Ninja graph for the esp_ipa component sometimes tries to compile
the generated esp_video_ipa_config.c before the custom generation step has
emitted it. Generating eagerly here keeps clean builds deterministic.

The sc2336 tuning JSON is per silicon revision, exactly as
espressif__esp_cam_sensor/project_include.cmake selects it:
CONFIG_ESP32P4_SELECTS_REV_LESS_V3 picks the eco4 file, otherwise eco5. This
script must mirror that, or the P4X (rev 3.x) env silently gets rev-<3 tuning.

NOTE: the related workaround for the stale dummy tuning table shipped inside
espressif__esp_ipa's prebuilt libesp_ipa.a lives in the project CMakeLists.txt,
not here. It has to run after project(), because the component manager
repopulates managed_components/ during configure and would undo a strip done at
this (script-parse) stage. See the comment there.
"""

from pathlib import Path
import subprocess
import sys

Import("env")

def _sdkconfig_has(sdkconfig: Path, key: str) -> bool:
    """True if `key=y` is set in this env's live sdkconfig."""
    if not sdkconfig.exists():
        return False
    needle = "{}=y".format(key)
    for line in sdkconfig.read_text(errors="replace").splitlines():
        if line.strip() == needle:
            return True
    return False


def _generate_ipa_config(source, target, env):
    project_dir = Path(env["PROJECT_DIR"])
    build_dir = Path(env.subst("$BUILD_DIR"))
    managed_dir = project_dir / "managed_components"
    generator = managed_dir / "espressif__esp_ipa" / "tools" / "config" / "esp_ipa_config.py"
    cam_sensor_dir = managed_dir / "espressif__esp_cam_sensor" / "sensors"
    output_dir = build_dir / "esp-idf" / "espressif__esp_ipa"
    output_file = output_dir / "esp_video_ipa_config.c"

    # Mirror project_include.cmake's per-revision choice.
    sdkconfig = project_dir / "sdkconfig.{}".format(env["PIOENV"])
    rev_less_v3 = _sdkconfig_has(sdkconfig, "CONFIG_ESP32P4_SELECTS_REV_LESS_V3")
    sc2336_cfg = "sc2336_default_p4_eco4.json" if rev_less_v3 else "sc2336_default_p4_eco5.json"

    inputs = [
        cam_sensor_dir / "ov5647" / "cfg" / "ov5647_default.json",
        cam_sensor_dir / "sc2336" / "cfg" / sc2336_cfg,
    ]

    existing_inputs = [str(path) for path in inputs if path.exists()]
    if not generator.exists() or not existing_inputs:
        return

    output_dir.mkdir(parents=True, exist_ok=True)
    cmd = [
        sys.executable,
        "-B",
        str(generator),
        "-i",
        " ".join(existing_inputs),
        "-o",
        str(output_file),
        "-v",
        "1",
    ]
    print("Generating esp_video_ipa_config.c (sc2336 tuning: {})".format(sc2336_cfg))
    subprocess.check_call(cmd, cwd=str(output_dir))


env.AddPreAction("buildprog", _generate_ipa_config)
_generate_ipa_config(None, None, env)
