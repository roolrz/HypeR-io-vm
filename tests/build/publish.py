#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 roolrz
# SPDX-License-Identifier: Apache-2.0
"""Reject incomplete or inconsistent boot packages before registry publication."""

import importlib.util
import json
import lzma
import io
import tarfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


FIXTURE = load('assembly_fixture', ROOT / 'tests/build/io-vm.py')
PUBLISH = load('publisher', ROOT / 'tools/publish.py')


class Publication(unittest.TestCase):
    def setUp(self):
        self.fixture = FIXTURE.Assembly()
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)
        self.args = self.fixture.args
        base = self.args.base
        config = b'CONFIG_ARM64=y\n'
        (base / 'kernel.config').write_bytes(config)
        metadata = json.loads((base / 'manifest.json').read_text())
        metadata.update(platform='qemu', kernel_release='6.18.51-test',
                        kernel_config_sha256=FIXTURE.ASSEMBLE.sha256(config))
        self.revision = 'a' * 40
        self.source_entries = {'hyper-io-vm/' + name: ('source ' + name).encode()
                               for name in PUBLISH.REQUIRED_SOURCES}
        self.source_entries['hyper-io-vm/rootfs/init'] = (self.args.overlay / 'init').read_bytes()
        self.source_entries['resolved/linux.config'] = config
        self.source_entries['resolved/busybox.config'] = b'busybox config'
        self.source_entries['resolved/compiler.txt'] = b'compiler identity'
        self.source_entries['upstream/linux.tar.xz'] = b'locked linux test source'
        self.source_entries['upstream/busybox.tar.bz2'] = b'locked busybox test source'
        metadata.update(source_revision=self.revision, source_dirty=False,
            busybox_config_sha256=FIXTURE.ASSEMBLE.sha256(self.source_entries['resolved/busybox.config']),
            compiler_sha256=FIXTURE.ASSEMBLE.sha256(self.source_entries['resolved/compiler.txt']),
            source_files={name: FIXTURE.ASSEMBLE.sha256(self.source_entries['hyper-io-vm/' + name])
                          for name in PUBLISH.REQUIRED_SOURCES},
            source_lock={name: {'url': 'https://example.invalid/' + filename,
                         'sha256': FIXTURE.ASSEMBLE.sha256(self.source_entries['upstream/' + filename])}
                         for name, filename in [('linux', 'linux.tar.xz'), ('busybox', 'busybox.tar.bz2')]})
        module_paths = ['include/hyper_io.h', 'include/hyper_io_layout.h', 'modules/guest-memory/Makefile',
                        'modules/guest-memory/hyper_guest_memory.c', 'modules/io-bridge/Makefile',
                        'modules/io-bridge/hyper_io_bridge.c']
        metadata['external_module_source_sha256'] = FIXTURE.ASSEMBLE.sha256(
            ''.join(metadata['source_files'][name] for name in module_paths).encode())
        data = json.dumps(metadata).encode()
        (base / 'manifest.json').write_bytes(data)
        self.args.manifest_sha256 = FIXTURE.ASSEMBLE.sha256(data)
        FIXTURE.ASSEMBLE.assemble(self.args)
        self.sources = base / 'sources.tar.xz'
        self.write_sources()
        self.output = base.parent / 'package'
        self.output.mkdir()

    def write_sources(self):
        with tarfile.open(self.sources, 'w:xz') as archive:
            for name, data in self.source_entries.items():
                info = tarfile.TarInfo(name)
                info.size = len(data)
                archive.addfile(info, io.BytesIO(data))

    def prepare(self):
        return PUBLISH.prepare(self.args.output, self.args.base, self.sources, self.output, self.revision)

    def test_common_capabilities_require_every_resolved_pi_builtin(self):
        config = ''.join('CONFIG_' + name + '=y\n' for name in PUBLISH.PI_BUILTINS).encode()
        metadata = {'platform': 'qemu'}
        self.assertEqual(PUBLISH.supported_platforms(metadata, config), ['qemu', 'rpi5'])
        for name in PUBLISH.PI_BUILTINS:
            with self.subTest(name=name):
                incomplete = config.replace(('CONFIG_' + name + '=y').encode(),
                                            ('CONFIG_' + name + '=m').encode())
                self.assertEqual(PUBLISH.supported_platforms(metadata, incomplete), ['qemu'])

    def test_coherent_boot_package_has_exact_four_payloads(self):
        metadata = self.prepare()
        self.assertEqual(metadata['kernel_release'], '6.18.51-test')
        self.assertEqual({p.name for p in self.output.iterdir()},
                         {'Image', 'initramfs.cpio.gz', 'sources.tar.xz', 'kernel.config'})

    def test_mismatched_config_or_boot_data_is_rejected(self):
        (self.args.base / 'kernel.config').write_bytes(b'other build')
        with self.assertRaises(ValueError):
            self.prepare()
        (self.args.output / 'boot-artifacts.json').write_text('{}')
        with self.assertRaises(KeyError):
            self.prepare()

    def test_wrong_source_bytes_and_revision_are_rejected(self):
        self.source_entries['hyper-io-vm/modules/io-bridge/hyper_io_bridge.c'] = b'wrong implementation'
        self.write_sources()
        with self.assertRaisesRegex(ValueError, 'source contents differ'):
            self.prepare()
        self.revision = 'b' * 40
        with self.assertRaisesRegex(ValueError, 'source revision'):
            self.prepare()

    def test_placeholder_xz_and_missing_upstream_are_rejected(self):
        self.sources.write_bytes(lzma.compress(b'not a source tar'))
        with self.assertRaises(ValueError):
            self.prepare()
        del self.source_entries['upstream/linux.tar.xz']
        self.write_sources()
        with self.assertRaisesRegex(ValueError, 'omits build inputs'):
            self.prepare()

    def test_boot_cannot_replace_a_qualified_base_binary(self):
        (self.args.overlay / 'bin').mkdir()
        (self.args.overlay / 'bin/sh').write_bytes(b'wrong binary')
        (self.args.overlay / 'bin/sh').chmod(0o755)
        FIXTURE.ASSEMBLE.assemble(self.args)
        with self.assertRaisesRegex(ValueError, 'base payload'):
            self.prepare()

    def test_missing_corresponding_sources_prevents_publication(self):
        self.sources.unlink()
        with self.assertRaises(ValueError):
            self.prepare()


if __name__ == '__main__':
    unittest.main()
