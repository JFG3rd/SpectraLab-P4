"""
Generate esp_video_ipa_config.c before compilation.

PlatformIO's Ninja graph for the Espressif esp_ipa component sometimes tries to
compile the generated C file before the custom generation step has emitted it.
Running the generator eagerly here keeps clean builds deterministic.
"""

from pathlib import Path
import subprocess
import sys

Import("env")


def _generate_ipa_config(source, target, env):
    project_dir = Path(env["PROJECT_DIR"])
    build_dir = Path(env.subst("$BUILD_DIR"))
    managed_dir = project_dir / "managed_components"
    generator = managed_dir / "espressif__esp_ipa" / "tools" / "config" / "esp_ipa_config.py"
    cam_sensor_dir = managed_dir / "espressif__esp_cam_sensor" / "sensors"
    output_dir = build_dir / "esp-idf" / "espressif__esp_ipa"
    output_file = output_dir / "esp_video_ipa_config.c"

    inputs = [
        cam_sensor_dir / "ov5647" / "cfg" / "ov5647_default.json",
        cam_sensor_dir / "sc2336" / "cfg" / "sc2336_default_p4_eco4.json",
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
    print("Generating esp_video_ipa_config.c")
    subprocess.check_call(cmd, cwd=str(output_dir))


env.AddPreAction("buildprog", _generate_ipa_config)
_generate_ipa_config(None, None, env)
