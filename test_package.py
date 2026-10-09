"""Verify package relocation, versioned backend files, and checksum sidecar."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest

from scripts.package import package


class PackageChecks(unittest.TestCase):
    def test_relocatable_runtime(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            build = root / "build"
            build.mkdir()
            (build / "libzvram_layer.so").write_bytes(b"fixture")
            (build / "zvram-codecs.json").write_text('{"bp16_gpu":false,"gdeflate_gpu":false}')
            (build / "VK_LAYER_NX_zvram.json").write_text('{"layer":{"library_path":"/old/build/libzvram_layer.so"}}')
            asset = package(build, root / "dist", "0.3.0")
            expected = hashlib.sha256(asset.read_bytes()).hexdigest()
            self.assertTrue(asset.with_name(asset.name + ".sha256").read_text().startswith(expected))
            stage = root / "relocated"
            with tarfile.open(asset) as archive:
                names = archive.getnames()
                self.assertTrue(all(name == "usr" or name.startswith("usr/") for name in names))
                self.assertFalse(any("gguf" in name.lower() or "__pycache__" in name for name in names))
                # Archive created entirely by this test from fixed, local package inputs.
                archive.extractall(stage)
            entry = stage / "usr/bin/zvram"
            self.assertEqual(entry.resolve(), stage / "usr/share/zvram/0.3.0/zvram")
            env = dict(os.environ, ZVRAM_MANAGER_HOME=str(root / "state"))
            result = subprocess.run([str(entry), "--version"], capture_output=True, text=True, check=True, env=env)
            self.assertEqual(result.stdout.strip(), "zVram 0.3.0")
            subprocess.run([str(entry), "manage", "list"], check=True, capture_output=True, env=env)
            self.assertFalse((entry.resolve().parent / "__pycache__").exists())
            manifest = json.loads((entry.resolve().parent / "build/VK_LAYER_NX_zvram.json").read_text())
            self.assertEqual(manifest["layer"]["library_path"], "./libzvram_layer.so")
            with self.assertRaises(ValueError):
                package(build, root / "dist", "../bad")

    def test_test_hook_build_refused(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "zvram-codecs.json").write_text('{"test_hooks":true}')
            with self.assertRaisesRegex(ValueError, "Test-hook builds"):
                package(root, root / "dist", "0.4.9")

    def test_missing_shader_refused(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "zvram-codecs.json").write_text('{"bp16_gpu":true}')
            for name in ("libzvram_layer.so", "VK_LAYER_NX_zvram.json"):
                (root / name).write_text("fixture")
            with self.assertRaisesRegex(ValueError, "bp16.spv"):
                package(root, root / "dist", "0.3.0")


if __name__ == "__main__":
    unittest.main()
