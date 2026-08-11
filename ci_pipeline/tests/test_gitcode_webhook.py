import hashlib
import io
from pathlib import Path
import sys
import tempfile
import unittest
import zipfile


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

    def test_release_manifest_accepts_the_expected_pair(self):
        wheels = [
            Path("cinderx-1.2.3-cp314-cp314-manylinux_2_28_aarch64.whl"),
            Path("cinderx-1.2.3-cp311-cp311-linux_aarch64.whl"),
        ]
        webhook.validate_release_manifest(wheels, io.StringIO())

    def test_release_manifest_rejects_a_missing_cp314_wheel(self):
        wheels = [Path("cinderx-1.2.3-cp311-cp311-linux_aarch64.whl")]
        with self.assertRaisesRegex(RuntimeError, "exactly one cp314"):
            webhook.validate_release_manifest(wheels, io.StringIO())

    def test_release_manifest_rejects_duplicate_cp311_wheels(self):
        wheels = [
            Path("cinderx-1.2.3-cp314-cp314-manylinux_2_28_aarch64.whl"),
            Path("cinderx-1.2.3-cp311-cp311-linux_aarch64.whl"),
            Path("cinderx-1.2.3-1-cp311-cp311-linux_aarch64.whl"),
        ]
        with self.assertRaisesRegex(RuntimeError, "exactly one cp311"):
            webhook.validate_release_manifest(wheels, io.StringIO())

    def test_release_manifest_rejects_version_mismatch(self):
        wheels = [
            Path("cinderx-1.2.3-cp314-cp314-manylinux_2_28_aarch64.whl"),
            Path("cinderx-1.2.4-cp311-cp311-linux_aarch64.whl"),
        ]
        with self.assertRaisesRegex(RuntimeError, "versions disagree"):
            webhook.validate_release_manifest(wheels, io.StringIO())

    def test_release_manifest_rejects_wrong_cp311_platform(self):
        wheels = [
            Path("cinderx-1.2.3-cp314-cp314-manylinux_2_28_aarch64.whl"),
            Path("cinderx-1.2.3-cp311-cp311-manylinux_2_28_aarch64.whl"),
        ]
        with self.assertRaisesRegex(RuntimeError, "unexpected platform"):
            webhook.validate_release_manifest(wheels, io.StringIO())

    def test_release_manifest_rejects_foreign_wheels(self):
        wheels = [
            Path("cinderx-1.2.3-cp314-cp314-manylinux_2_28_aarch64.whl"),
            Path("cinderx-1.2.3-cp311-cp311-linux_aarch64.whl"),
            Path("cinderx-1.2.3-cp312-cp312-linux_aarch64.whl"),
        ]
        with self.assertRaisesRegex(RuntimeError, "unexpected wheels"):
            webhook.validate_release_manifest(wheels, io.StringIO())

    def test_release_manifest_rejects_wrong_abi(self):
        for wheels in (
            [
                Path("cinderx-1.2.3-cp314-abi3-manylinux_2_28_aarch64.whl"),
                Path("cinderx-1.2.3-cp311-cp311-linux_aarch64.whl"),
            ],
            [
                Path("cinderx-1.2.3-cp314-cp314-manylinux_2_28_aarch64.whl"),
                Path("cinderx-1.2.3-cp311-abi3-linux_aarch64.whl"),
            ],
        ):
            with self.assertRaisesRegex(RuntimeError, "unexpected abi"):
                webhook.validate_release_manifest(wheels, io.StringIO())

    def test_release_manifest_rejects_fabricated_platform(self):
        wheels = [
            Path("cinderx-1.2.3-cp314-cp314-notmanylinux_but_aarch64.whl"),
            Path("cinderx-1.2.3-cp311-cp311-linux_aarch64.whl"),
        ]
        with self.assertRaisesRegex(RuntimeError, "unexpected platform"):
            webhook.validate_release_manifest(wheels, io.StringIO())

    @staticmethod
    def _write_fat_wheel(path, version="1.2.3", variants=("py314_0", "py314_1", "py314_2", "py314_3"),
                         loader=True, top_level_so=False):
        with zipfile.ZipFile(path, "w") as wheel:
            if loader:
                wheel.writestr("_cinderx.py", "# loader")
            wheel.writestr("cinderx/_native/fat_wheel.json", "{}")
            for variant in variants:
                wheel.writestr(f"cinderx/_native/{variant}/_cinderx.so", b"native")
            if top_level_so:
                wheel.writestr("_cinderx.so", b"native")
            wheel.writestr(
                f"cinderx-{version}.dist-info/METADATA",
                f"Metadata-Version: 2.1\nName: cinderx\nVersion: {version}\n",
            )

    def test_fat_wheel_content_accepts_a_real_fat_layout(self):
        with tempfile.TemporaryDirectory() as tmp:
            wheel = Path(tmp) / "cinderx-1.2.3-cp314-cp314-manylinux_2_28_aarch64.whl"
            self._write_fat_wheel(wheel)
            webhook.validate_fat_wheel_content(wheel, "1.2.3", io.StringIO())

    def test_fat_wheel_content_rejects_an_ordinary_wheel(self):
        with tempfile.TemporaryDirectory() as tmp:
            wheel = Path(tmp) / "cinderx-1.2.3-cp314-cp314-manylinux_2_28_aarch64.whl"
            with zipfile.ZipFile(wheel, "w") as zf:
                zf.writestr("_cinderx.so", b"native")
                zf.writestr(
                    "cinderx-1.2.3.dist-info/METADATA",
                    "Metadata-Version: 2.1\nName: cinderx\nVersion: 1.2.3\n",
                )
            with self.assertRaisesRegex(RuntimeError, "fat loader"):
                webhook.validate_fat_wheel_content(wheel, "1.2.3", io.StringIO())

    def test_fat_wheel_content_rejects_a_missing_variant(self):
        with tempfile.TemporaryDirectory() as tmp:
            wheel = Path(tmp) / "cinderx-1.2.3-cp314-cp314-manylinux_2_28_aarch64.whl"
            self._write_fat_wheel(wheel, variants=("py314_0", "py314_1", "py314_2"))
            with self.assertRaisesRegex(RuntimeError, "native variants"):
                webhook.validate_fat_wheel_content(wheel, "1.2.3", io.StringIO())

    def test_fat_wheel_content_rejects_version_mismatch(self):
        with tempfile.TemporaryDirectory() as tmp:
            wheel = Path(tmp) / "cinderx-1.2.3-cp314-cp314-manylinux_2_28_aarch64.whl"
            self._write_fat_wheel(wheel, version="1.2.4")
            with self.assertRaisesRegex(RuntimeError, "METADATA version"):
                webhook.validate_fat_wheel_content(wheel, "1.2.3", io.StringIO())

    def test_release_body_python_line_follows_the_actual_assets(self):
        cp314_only = [
            {
                "name": "cinderx-1.2.3-cp314-cp314-manylinux_2_28_aarch64.whl",
                "sha256": "a" * 64,
                "sha256_name": "cinderx-1.2.3-cp314-cp314-manylinux_2_28_aarch64.whl.sha256",
            }
        ]
        body = webhook.build_release_body("v1.2.3", "b" * 40, io.StringIO(), cp314_only)
        self.assertIn("CPython 3.14", body)
        self.assertNotIn("CPython 3.11", body)

    def test_release_manifest_rejects_cp311_when_disabled(self):
        wheels = [
            Path("cinderx-1.2.3-cp314-cp314-manylinux_2_28_aarch64.whl"),
            Path("cinderx-1.2.3-cp311-cp311-linux_aarch64.whl"),
        ]
        previous = webhook.RELEASE_CP311
        webhook.RELEASE_CP311 = False
        try:
            with self.assertRaisesRegex(RuntimeError, "CINDERX_RELEASE_CP311 is off"):
                webhook.validate_release_manifest(wheels, io.StringIO())
        finally:
            webhook.RELEASE_CP311 = previous

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
