"""Exercise the native bake toolbar in a normal editor, then reopen the scene."""

from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile


project = Path(__file__).resolve().parent
engine = Path(sys.argv[1]).resolve()
plugin = project / "addons/lighting_bake_validation"
settings = project / "project.godot"
original = settings.read_bytes()
assert b"[editor_plugins]" not in original, "Run in the unmodified regression project"
assert not plugin.exists(), "Refusing to overwrite an existing plugin"
injected = original + b'\n[editor_plugins]\nenabled=PackedStringArray("res://addons/lighting_bake_validation/plugin.cfg")\n'
plugin.mkdir(parents=True)
try:
    (plugin / "plugin.cfg").write_text(
        '[plugin]\nname="Lighting Bake Validation"\ndescription="Temporary regression runner"\n'
        'author="Godot"\nversion="1.0"\nscript="validation.gd"\n'
    )
    (plugin / "validation.gd").write_bytes((project / "editor_smoke.gd").read_bytes())
    settings.write_bytes(injected)
    with tempfile.TemporaryDirectory(prefix="godot-lighting-editor-") as runtime:
        env = dict(os.environ, APPDATA=runtime, LOCALAPPDATA=runtime)
        for mode in ["bake", "verify"]:
            args = [str(engine), "--editor", "--path", str(project), "--log-file", str(project / f"editor_smoke_{mode}.log"), "--disable-vsync", "--max-fps", "60", "--"]
            if mode == "verify":
                args.append("--editor-verify")
            result = subprocess.run(args, env=env, timeout=300, check=True)
            print(f"Native editor {mode}: exit {result.returncode}", flush=True)
finally:
    if settings.read_bytes() == injected:
        settings.write_bytes(original)
    else:
        # The editor can rewrite project settings while saving the test scene.
        # Remove only this runner's plugin entry, preserving other settings.
        current = settings.read_bytes()
        entry = b'"res://addons/lighting_bake_validation/plugin.cfg"'
        current = current.replace(entry + b', ', b'').replace(b', ' + entry, b'').replace(entry, b'')
        current = re.sub(rb'\[editor_plugins\]\r?\n\s*enabled=PackedStringArray\(\)\r?\n\s*', b'', current)
        settings.write_bytes(current)
    for name in ["validation.gd", "validation.gd.uid", "plugin.cfg"]:
        (plugin / name).unlink(missing_ok=True)
    plugin.rmdir()
