import os
from pathlib import Path
import subprocess
import sys
import textwrap
import unittest


PYTHONLIB = Path(__file__).parents[2]


class CinderXAutoBootstrapTest(unittest.TestCase):
    def test_plugin_startup_uses_lightweight_bootstrap(self) -> None:
        with self.subTest("does not import the full cinderx package"):
            import tempfile

            with tempfile.TemporaryDirectory() as tempdir:
                temp = Path(tempdir)
                (temp / "cinderx.py").write_text(
                    "raise AssertionError('full cinderx package imported')\n"
                )
                (temp / "_cinderx.py").write_text(
                    textwrap.dedent(
                        """
                        def install_frame_evaluator():
                            pass

                        def _autojit_import_enter():
                            pass

                        def _autojit_import_leave():
                            pass

                        def _autojit_import_depth():
                            return 0
                        """
                    )
                )
                (temp / "cinderjit.py").write_text(
                    textwrap.dedent(
                        """
                        def is_enabled():
                            return True
                        """
                    )
                )

                env = os.environ.copy()
                env["CINDERX_PLUGIN_ENABLE"] = "1"
                env["PYTHONJITAUTO"] = "auto:2"
                env["PYTHONPATH"] = f"{temp}{os.pathsep}{PYTHONLIB}"

                completed = subprocess.run(
                    [
                        sys.executable,
                        "-S",
                        "-c",
                        textwrap.dedent(
                            """
                            import sys
                            import _cinderx_auto

                            assert "cinderx" not in sys.modules
                            assert "_cinderx" in sys.modules
                            assert "cinderjit" in sys.modules
                            """
                        ),
                    ],
                    env=env,
                    capture_output=True,
                    text=True,
                    timeout=30,
                )

                self.assertEqual(
                    completed.returncode,
                    0,
                    f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}",
                )


if __name__ == "__main__":
    unittest.main()
