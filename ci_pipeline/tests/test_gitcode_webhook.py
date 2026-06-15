import hashlib
import io
from pathlib import Path
import sys
import tempfile
import unittest


REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT))

from ci_pipeline.build_with_tag import gitcode_webhook as webhook  # noqa: E402


class GitCodeWebhookTests(unittest.TestCase):
    def setUp(self):
        self._previous_release_tag = webhook.previous_release_tag
        self._git_log_entries = webhook.git_log_entries
        webhook.previous_release_tag = lambda tag, log: "v1.2.2"
        webhook.git_log_entries = lambda previous_tag, log: [
            "abc123 feat(jit): add fast path",
        ]

    def tearDown(self):
        webhook.previous_release_tag = self._previous_release_tag
        webhook.git_log_entries = self._git_log_entries

    def test_collect_wheel_assets_writes_sha256_sidecar(self):
        with tempfile.TemporaryDirectory() as tmp:
            wheel = Path(tmp) / "cinderx-1.2.3-cp314-cp314-manylinux.whl"
            data = b"wheel bytes"
            wheel.write_bytes(data)

            assets = webhook.collect_wheel_assets([wheel])

            expected = hashlib.sha256(data).hexdigest()
            self.assertEqual(len(assets), 1)
            self.assertEqual(assets[0]["name"], wheel.name)
            self.assertEqual(assets[0]["sha256"], expected)
            sidecar = Path(assets[0]["sha256_path"])
            self.assertEqual(sidecar.name, f"{wheel.name}.sha256")
            self.assertEqual(sidecar.read_text(encoding="utf-8"), f"{expected}  {wheel.name}\n")

    def test_release_body_includes_hash_checked_install_instructions(self):
        digest = "a" * 64
        wheel_name = "cinderx-1.2.3-cp314-cp314-manylinux.whl"
        body = webhook.build_release_body(
            "v1.2.3",
            "b" * 40,
            io.StringIO(),
            [
                {
                    "name": wheel_name,
                    "sha256": digest,
                    "sha256_name": f"{wheel_name}.sha256",
                }
            ],
        )

        self.assertIn("## 完整性校验", body)
        self.assertIn("sha256sum -c <wheel-file>.sha256", body)
        self.assertIn("--require-hashes", body)
        self.assertIn(f"./{wheel_name} --hash=sha256:{digest}", body)
        self.assertIn(f"`{wheel_name}`: `{digest}`", body)


if __name__ == "__main__":
    unittest.main()
