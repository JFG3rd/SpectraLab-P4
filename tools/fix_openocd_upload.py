"""
Work around a bug in platform-espressif32's OpenOCD upload command
construction (builder/main.py, upload_protocol esp-builtin/openocd path).

It emits `program_esp {{$SOURCE}} <offset> verify`. Tcl only strips one
brace level per word, so OpenOCD receives a path literally wrapped in
`{` `}` characters and fails with "couldn't open {...path...}". Stripping
the doubled braces here lets the existing flash images upload correctly.

Remove this once upstream fixes the brace doubling.
"""
Import("env")


def _fix_openocd_braces(source, target, env):
    flags = env.get("UPLOADERFLAGS")
    if not flags:
        return
    env["UPLOADERFLAGS"] = [
        f.replace("{{", "{").replace("}}", "}") if isinstance(f, str) else f
        for f in flags
    ]


env.AddPreAction("upload", _fix_openocd_braces)
