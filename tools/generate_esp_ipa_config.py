"""
Make the camera's ISP tuning (IPA) configuration actually reach the firmware.

Two separate problems are handled here. Both stop the camera dead with the same
symptom: VIDIOC_STREAMON fails and the QR screen reports "The camera refused to
start streaming".

1. Ninja race. PlatformIO's Ninja graph for the esp_ipa component sometimes
   tries to compile the generated esp_video_ipa_config.c before the custom
   generation step has emitted it. Generating eagerly here keeps clean builds
   deterministic.

   The JSON tuning file is per silicon revision, exactly as
   espressif__esp_cam_sensor/project_include.cmake selects it:
   CONFIG_ESP32P4_SELECTS_REV_LESS_V3 picks the sc2336 eco4 file, otherwise
   eco5. This script must mirror that, or the P4X (rev 3.x) env silently gets
   the rev-<3 tuning.

2. Duplicate symbol in the prebuilt library. espressif__esp_ipa ships
   lib/<target>/<idf>/libesp_ipa.a containing a stale, EMPTY
   esp_video_ipa_config.c.obj — a placeholder built with no JSON input, whose
   esp_ipa_pipeline_get_config() returns NULL for every sensor. Both that
   archive and the component's own libespressif__esp_ipa.a define the symbol,
   and under PlatformIO's link order the prebuilt one wins.

   The result: esp_video_init() logs "failed to get configuration to initialize
   ISP controller", the ISP pipeline is never configured, white-balance gains
   are never set, and esp_isp_wbg_set_wb_gain() then fails ->
   isp_start_pipeline -> csi_video_start -> STREAMON fails.

   Deleting the stale member from the prebuilt archive leaves exactly one
   definition, so the generated table is the one that links. Idempotent: on
   later builds the member is already gone.
"""

from pathlib import Path
import subprocess
import sys

Import("env")

IPA_STALE_OBJ = "esp_video_ipa_config.c.obj"


def _sdkconfig_has(sdkconfig: Path, key: str) -> bool:
    """True if `key=y` is set in this env's live sdkconfig."""
    if not sdkconfig.exists():
        return False
    needle = "{}=y".format(key)
    for line in sdkconfig.read_text(errors="replace").splitlines():
        if line.strip() == needle:
            return True
    return False


def _strip_stale_ipa_object(managed_dir: Path) -> None:
    """Remove the empty placeholder config object from the prebuilt archive."""
    ar = env.subst("$AR") or "riscv32-esp-elf-ar"

    for archive in sorted(managed_dir.glob("espressif__esp_ipa/lib/*/*/libesp_ipa.a")):
        try:
            members = subprocess.check_output([ar, "t", str(archive)], text=True)
        except (subprocess.CalledProcessError, OSError) as exc:
            print("esp_ipa: could not read {}: {}".format(archive.name, exc))
            continue

        if IPA_STALE_OBJ not in members.split():
            continue  # already stripped

        try:
            subprocess.check_call([ar, "d", str(archive), IPA_STALE_OBJ])
            print("esp_ipa: removed stale {} from {} — it shadows the "
                  "generated sensor tuning table".format(IPA_STALE_OBJ, archive.name))
        except (subprocess.CalledProcessError, OSError) as exc:
            print("esp_ipa: failed to strip {}: {}".format(archive.name, exc))


def _generate_ipa_config(source, target, env):
    project_dir = Path(env["PROJECT_DIR"])
    build_dir = Path(env.subst("$BUILD_DIR"))
    managed_dir = project_dir / "managed_components"
    generator = managed_dir / "espressif__esp_ipa" / "tools" / "config" / "esp_ipa_config.py"
    cam_sensor_dir = managed_dir / "espressif__esp_cam_sensor" / "sensors"
    output_dir = build_dir / "esp-idf" / "espressif__esp_ipa"
    output_file = output_dir / "esp_video_ipa_config.c"

    _strip_stale_ipa_object(managed_dir)

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
