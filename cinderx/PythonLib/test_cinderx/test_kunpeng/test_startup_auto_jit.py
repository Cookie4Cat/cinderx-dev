import os
from pathlib import Path
import subprocess
import sys

HELPER = Path(__file__).with_name("startup_auto_jit_helper.py")


def test_installed_cinderx_auto_imports_and_jits(tmp_path):
    """Requires cinderx to be installed into the tested interpreter."""
    env = os.environ.copy()
    env.pop("PYTHONPATH", None)
    for name in ("CINDERX_DISABLE", "CINDERX_JIT_DISABLE", "PYTHONJITDISABLE"):
        env.pop(name, None)

    env["CINDERX_PLUGIN_ENABLE"] = "1"
    env["PYTHONJITALL"] = "1"

    completed = subprocess.run(
        [sys.executable, str(HELPER)],
        # Keep the child away from the source tree so startup must use the
        # installed cinderx package rather than local imports.
        cwd=tmp_path,
        env=env,
        capture_output=True,
        text=True,
        timeout=60,
    )
    assert completed.returncode == 0, (
        "installed cinderx auto-import JIT subprocess failed\n"
        f"stdout:\n{completed.stdout}\n"
        f"stderr:\n{completed.stderr}"
    )
    assert "Traceback" not in completed.stderr
