#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 roolrz
# SPDX-License-Identifier: Apache-2.0

"""I/O VM archive boundary and immutable assembly regressions."""

import gzip
import importlib.util
import json
from pathlib import Path
import stat
import tempfile
from types import SimpleNamespace
import unittest


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("assemble_io_vm", ROOT / "scripts/assemble-io-vm.py")
ASSEMBLE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ASSEMBLE)


def directory():
    return (stat.S_IFDIR | 0o755, 0, 0, 0, 0, b"")


def regular(contents, mode=0o644):
    return (stat.S_IFREG | mode, 0, 0, 0, 0, contents)


class Archives(unittest.TestCase):
    def test_round_trip_preserves_types_modes_and_data(self):
        entries = {"bin": directory(), "bin/busybox": regular(b"ELF", 0o755),
                   "bin/sh": (stat.S_IFLNK | 0o777, 0, 0, 0, 0, b"busybox"),
                   "dev": directory(),
                   "dev/console": (stat.S_IFCHR | 0o600, 0, 0, 5, 1, b"")}
        archive = ASSEMBLE.encode_newc(entries, 4096)
        self.assertEqual(ASSEMBLE.parse_newc(archive), entries)
        self.assertEqual(archive, ASSEMBLE.encode_newc(dict(reversed(list(entries.items()))), 4096))

    def test_truncation_and_trailing_archive_rejected(self):
        archive = ASSEMBLE.encode_newc({"file": regular(b"hello")}, 4096)
        for length in range(len(archive)):
            with self.subTest(length=length), self.assertRaises(ValueError):
                ASSEMBLE.parse_newc(archive[:length])
        with self.assertRaises(ValueError):
            ASSEMBLE.parse_newc(archive + archive)

    def test_symlink_parent_and_traversal_rejected(self):
        for name in ("../escape", "/absolute", "a/./b", "a//b", "a/../b"):
            with self.subTest(name=name), self.assertRaises(ValueError):
                ASSEMBLE.archive_name(name)
        entries = {"etc": (stat.S_IFLNK | 0o777, 0, 0, 0, 0, b"/elsewhere"),
                   "etc/config": regular(b"secret")}
        with self.assertRaises(ValueError):
            ASSEMBLE.encode_newc(entries, 4096)

    def test_bounded_gzip(self):
        compressed = gzip.compress(b"a" * 8192, mtime=0)
        self.assertEqual(ASSEMBLE.inflate(compressed, 8192), b"a" * 8192)
        for data, limit in ((compressed, 8191), (compressed[:-1], 8192),
                            (compressed + compressed, 16384)):
            with self.assertRaises(ValueError):
                ASSEMBLE.inflate(data, limit)


class Assembly(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        root = Path(self.temporary.name)
        base, overlay = root / "base", root / "overlay"
        base.mkdir()
        overlay.mkdir()
        kernel = bytearray(64)
        kernel[56:60] = b"ARM\x64"
        (base / "Image").write_bytes(kernel)
        archive = ASSEMBLE.encode_newc({"bin": directory(), "bin/sh": regular(b"shell", 0o755)}, 4096)
        (base / "base.cpio.gz").write_bytes(gzip.compress(archive, mtime=0))
        manifest = {"format": 1, "architecture": "aarch64", "files": {}}
        for name in ("Image", "base.cpio.gz"):
            data = (base / name).read_bytes()
            manifest["files"][name] = {"size": len(data), "sha256": ASSEMBLE.sha256(data)}
        payload = json.dumps(manifest).encode()
        (base / "manifest.json").write_bytes(payload)
        (overlay / "init").write_text("#!/bin/sh\nexec /bin/sh\n")
        (overlay / "init").chmod(0o755)
        self.args = SimpleNamespace(base=base, overlay=overlay, output=root / "output",
                                    manifest_sha256=ASSEMBLE.sha256(payload), architecture="aarch64",
                                    kernel_limit=4096, compressed_limit=4096, expanded_limit=8192)

    def test_immutable_reproducible_assembly_and_overlay_update(self):
        before = {p.name: p.read_bytes() for p in self.args.base.iterdir()}
        result = ASSEMBLE.assemble(self.args)
        path = self.args.output / result["initramfs"]
        timestamp = path.stat().st_mtime_ns
        self.assertEqual(ASSEMBLE.assemble(self.args), result)
        self.assertEqual(path.stat().st_mtime_ns, timestamp)
        self.assertEqual(before, {p.name: p.read_bytes() for p in self.args.base.iterdir()})
        entries = ASSEMBLE.parse_newc(gzip.decompress(path.read_bytes()))
        self.assertIn("init", entries)
        self.assertIn("bin/sh", entries)
        (self.args.overlay / "init").write_text("#!/bin/sh\nexit 1\n")
        replacement = ASSEMBLE.assemble(self.args)
        self.assertNotEqual(result["initramfs"], replacement["initramfs"])
        self.assertEqual(result["kernel"], replacement["kernel"])
        self.assertTrue(path.exists())
        self.assertEqual(json.loads((self.args.output / "boot-artifacts.json").read_bytes()), replacement)

    def test_bad_digest_does_not_publish(self):
        self.args.manifest_sha256 = "0" * 64
        with self.assertRaises(ValueError):
            ASSEMBLE.assemble(self.args)
        self.assertFalse(self.args.output.exists())

    def test_tampered_artifact_does_not_publish(self):
        (self.args.base / "Image").write_bytes(b"tampered")
        with self.assertRaises(ValueError):
            ASSEMBLE.assemble(self.args)
        self.assertFalse(self.args.output.exists())

    def test_launch_policy_must_come_from_overlay(self):
        (self.args.overlay / "init").unlink()
        with self.assertRaises(ValueError):
            ASSEMBLE.assemble(self.args)

    def test_budget_failure_preserves_previous_generation(self):
        ASSEMBLE.assemble(self.args)
        manifest = (self.args.output / "boot-artifacts.json").read_bytes()
        self.args.expanded_limit = 1
        with self.assertRaises(ValueError):
            ASSEMBLE.assemble(self.args)
        self.assertEqual(manifest, (self.args.output / "boot-artifacts.json").read_bytes())


if __name__ == "__main__":
    unittest.main()
