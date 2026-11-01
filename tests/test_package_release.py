"""Release export boundaries and final artifact integrity; no SDK or hardware required."""
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest
import zipfile

SPEC = importlib.util.spec_from_file_location(
    "package_release", Path(__file__).resolve().parents[1] / "scripts/package_release.py")
pkg = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(pkg)


class PackageTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        subprocess.run(["git", "init", "-q", str(self.root)], check=True)
        self.esp = self.root / "build/release/esp32"
        self.esp.mkdir(parents=True)
        merged = bytearray(b"\xff" * 0x20020)
        for index, (offset, name) in enumerate(pkg.FLASH_FILES.items()):
            path = self.esp / name
            path.parent.mkdir(parents=True, exist_ok=True)
            data = bytes([0xe9 if index == 0 else index]) * 32
            path.write_bytes(data)
            start = int(offset, 16)
            merged[start:start + len(data)] = data
        (self.esp / "merged-binary.bin").write_bytes(merged)
        self.manifest = {"flash_files": pkg.FLASH_FILES,
                         "flash_settings": {"flash_mode": "dio", "flash_size": "16MB", "flash_freq": "80m"},
                         "extra_esptool_args": {"chip": "esp32s3"}, "private_key": "DO-NOT-EXPORT"}
        self.write_manifest()
        for name in ("xiaozhi.elf", "device-token.txt", "server.env"):
            (self.esp / name).write_text("DO-NOT-EXPORT")
        fz = self.root / "build/release/flipper"
        update = fz / "f7-update-local"
        update.mkdir(parents=True)
        for name in pkg.UPDATE_FILES:
            (update / name).write_bytes(name.encode())
        with tarfile.open(fz / "flipper-z-f7-update-local.tgz", "w:gz") as tar:
            tar.add(update, arcname="f7-update-local")
        (fz / "firmware.bin").write_bytes(b"firmware")
        report = {"esp32": {"merged_sha256": pkg.sha(merged)},
                  "flipper": {"sha256": pkg.sha(b"firmware")}}
        (self.root / "build/validation.json").write_bytes(pkg.json_bytes(report))

    def write_manifest(self):
        (self.esp / "flasher_args.json").write_bytes(pkg.json_bytes(self.manifest))

    def test_dirty_refused(self):
        with self.assertRaisesRegex(ValueError, "dirty"):
            pkg.package(self.root, "v1.0.0")

    def test_traversal_refused(self):
        self.manifest["flash_files"] = {"0x0": "../private.bin"}
        self.write_manifest()
        with self.assertRaisesRegex(ValueError, "escaping"):
            pkg.esp_payload(self.esp)

    def test_nvs_refused(self):
        path = self.esp / "merged-binary.bin"
        data = bytearray(path.read_bytes())
        data[0x9000] = 0
        path.write_bytes(data)
        with self.assertRaisesRegex(ValueError, "NVS"):
            pkg.esp_payload(self.esp)

    def test_export_whitelist_and_checksums(self):
        # A real HEAD is required even for preview packaging.
        subprocess.run(["git", "-C", str(self.root), "-c", "user.name=Test", "-c",
                        "user.email=test@example.invalid", "commit", "--allow-empty", "-qm", "test"], check=True)
        dest = pkg.package(self.root, "v1.0.0-test.1", allow_dirty=True)
        with zipfile.ZipFile(next(dest.glob("*.zip"))) as archive:
            self.assertEqual(set(archive.namelist()), set(pkg.FLASH_FILES.values())
                             | {"merged-binary.bin", "flasher_args.json", "烧录说明.md"})
            self.assertNotIn(b"DO-NOT-EXPORT", archive.read("flasher_args.json"))
        for line in (dest / "SHA256SUMS.txt").read_text().splitlines():
            digest, name = line.split("  ", 1)
            self.assertEqual(digest, hashlib.sha256((dest / name).read_bytes()).hexdigest())
        state = json.loads((dest / "SOURCE_STATE.json").read_text())
        self.assertTrue(state["dirty"])
        self.assertNotIn(str(self.root), json.dumps(state))
        with self.assertRaisesRegex(ValueError, "already exists"):
            pkg.package(self.root, "v1.0.0-test.1", allow_dirty=True)


if __name__ == "__main__":
    unittest.main()
