#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 roolrz
# SPDX-License-Identifier: Apache-2.0
"""Publish a complete boot image and corresponding sources as an OCI artifact."""

import argparse
import hashlib
import importlib.util
import json
import re
import shutil
import subprocess
import tempfile
import tarfile
import lzma
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('assemble', ROOT / 'scripts/assemble-io-vm.py')
ARCHIVE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ARCHIVE)


REQUIRED_SOURCES = {
    'Makefile', 'tools/build.py', 'scripts/assemble-io-vm.py', 'rootfs/init',
    'include/hyper_io.h', 'include/hyper_io_layout.h', 'include/hyper_io_session.h',
    'service/hyper-volumes.c', 'service/hyper-io-supervisor.c', 'modules/guest-memory/Makefile',
    'modules/guest-memory/hyper_guest_memory.c', 'modules/io-bridge/Makefile',
    'modules/io-bridge/hyper_io_bridge.c', 'service/hyper-io-service.c',
    'tests/io-vm/business-disk.c', 'sources.lock.json',
    'configs/linux-aarch64.config', 'configs/busybox.config', 'LICENSE', 'LICENSES/GPL-2.0-only.txt',
}


# Capability metadata describes the same DT-selected binary, not hardware
# qualification. A Pi deployment remains unqualified until physical acceptance.
PI_BUILTINS = (
    'ARM64', 'ARCH_BCM', 'ARCH_BCM2835', 'COMMON_CLK', 'PINCTRL',
    'PINCTRL_BRCMSTB', 'PINCTRL_BCM2712', 'GPIOLIB', 'OF_GPIO', 'GPIO_BRCMSTB',
    'REGULATOR', 'REGULATOR_FIXED_VOLTAGE', 'REGULATOR_GPIO', 'MMC', 'MMC_BLOCK',
    'MMC_SDHCI', 'MMC_SDHCI_PLTFM', 'MMC_SDHCI_BRCMSTB', 'MMC_CQHCI',
)


def supported_platforms(metadata, config):
    values = dict(line.split('=', 1) for line in config.decode().splitlines()
                  if line.startswith('CONFIG_') and '=' in line)
    result = [metadata['platform']]
    if metadata['platform'] == 'qemu' and all(values.get('CONFIG_' + name) == 'y'
                                            for name in PI_BUILTINS):
        result.append('rpi5')
    return result


def verify_sources(sources, metadata, revision):
    if (metadata.get('source_revision') != revision or metadata.get('source_dirty') is not False):
        raise ValueError('publication requires the exact clean build source revision')
    hashes = metadata.get('source_files', {})
    if not REQUIRED_SOURCES <= hashes.keys():
        raise ValueError('build manifest lacks required source identities')
    module_paths = ['include/hyper_io.h', 'include/hyper_io_layout.h', 'modules/guest-memory/Makefile',
                    'modules/guest-memory/hyper_guest_memory.c', 'modules/io-bridge/Makefile',
                    'modules/io-bridge/hyper_io_bridge.c']
    module_identity = hashlib.sha256(''.join(hashes[name] for name in module_paths).encode()).hexdigest()
    if module_identity != metadata['external_module_source_sha256']:
        raise ValueError('module sources differ from the compiled module identity')
    if metadata['platform'] == 'rpi5' and 'configs/linux-rpi5.config' not in hashes:
        raise ValueError('missing board build input')
    expected = {'hyper-io-vm/' + name: value for name, value in hashes.items()}
    expected['resolved/linux.config'] = metadata['kernel_config_sha256']
    expected['resolved/busybox.config'] = metadata['busybox_config_sha256']
    expected['resolved/compiler.txt'] = metadata['compiler_sha256']
    for component in ('linux', 'busybox'):
        identity = metadata['source_lock'][component]
        expected['upstream/' + identity['url'].rsplit('/', 1)[1]] = identity['sha256']
    if any(not re.fullmatch(r'[0-9a-f]{64}', value) for value in expected.values()):
        raise ValueError('invalid source hash')
    remaining = set(expected)
    seen = set()
    total = 0
    try:
        with tarfile.open(sources, 'r|xz') as archive:
            for member in archive:
                total += member.size
                if (not member.isfile() or Path(member.name).is_absolute() or '..' in Path(member.name).parts
                        or member.name in seen or member.size < 0
                        or total > 2 * 1024 * ARCHIVE.MIB):
                    raise ValueError('invalid or excessive source archive entries')
                seen.add(member.name)
                if member.name in expected:
                    with archive.extractfile(member) as data:
                        digest = hashlib.sha256()
                        for chunk in iter(lambda: data.read(1024 * 1024), b''):
                            digest.update(chunk)
                        actual = digest.hexdigest()
                    if actual != expected[member.name]:
                        raise ValueError('source contents differ from the build: ' + member.name)
                    remaining.remove(member.name)
    except (tarfile.TarError, lzma.LZMAError, EOFError) as error:
        raise ValueError('corresponding sources must be a valid tar.xz') from error
    if remaining:
        raise ValueError('source archive omits build inputs: ' + ', '.join(sorted(remaining)))


def prepare(boot, base, sources, output, revision):
    boot_manifest = json.loads((boot / 'boot-artifacts.json').read_text())
    base_bytes = (base / 'manifest.json').read_bytes()
    if hashlib.sha256(base_bytes).hexdigest() != boot_manifest['base_manifest_sha256']:
        raise ValueError('boot image and base manifest do not match')
    metadata = json.loads(base_bytes)
    if (boot_manifest['format'] != 1 or metadata['format'] != 1
            or boot_manifest['architecture'] != 'aarch64'
            or metadata['architecture'] != 'aarch64'
            or metadata['platform'] not in ('qemu', 'rpi5')):
        raise ValueError('unsupported boot image')
    base_ramdisk = ARCHIVE.bounded_read(base / 'base.cpio.gz', 8 * ARCHIVE.MIB)
    if ARCHIVE.sha256(base_ramdisk) != metadata['files']['base.cpio.gz']['sha256']:
        raise ValueError('base ramdisk differs from its build manifest')
    base_entries = ARCHIVE.parse_newc(ARCHIVE.inflate(base_ramdisk, 32 * ARCHIVE.MIB))
    for field, pattern, name, limit in (
        ('kernel', r'Image-([0-9a-f]{64})', 'Image', 32 * ARCHIVE.MIB),
        ('initramfs', r'initramfs-([0-9a-f]{64})\.cpio\.gz', 'initramfs.cpio.gz', 8 * ARCHIVE.MIB),
    ):
        match = re.fullmatch(pattern, boot_manifest[field])
        if match is None:
            raise ValueError('invalid content-addressed boot filename')
        data = ARCHIVE.bounded_read(boot / boot_manifest[field], limit)
        if ARCHIVE.sha256(data) != match[1]:
            raise ValueError('boot artifact checksum mismatch')
        if field == 'kernel':
            if (data[56:60] != b'ARM\x64'
                    or ARCHIVE.sha256(data) != metadata['files']['Image']['sha256']):
                raise ValueError('kernel does not match the base')
        else:
            entries = ARCHIVE.parse_newc(ARCHIVE.inflate(data, 32 * ARCHIVE.MIB))
            import stat
            if ('init' not in entries or not stat.S_ISREG(entries['init'][0])
                    or not entries['init'][0] & 0o111):
                raise ValueError('a complete initramfs with executable /init is required')
            for path, original in base_entries.items():
                if not stat.S_ISDIR(original[0]) and entries.get(path) != original:
                    raise ValueError('boot replaces a qualified base payload: ' + path)
            if ARCHIVE.sha256(entries['init'][-1]) != metadata.get('source_files', {}).get('rootfs/init'):
                raise ValueError('boot init differs from the recorded build source')
        (output / name).write_bytes(data)
    config = ARCHIVE.bounded_read(base / 'kernel.config', ARCHIVE.MIB)
    if ARCHIVE.sha256(config) != metadata['kernel_config_sha256']:
        raise ValueError('kernel configuration does not match the base')
    (output / 'kernel.config').write_bytes(config)
    if not sources.is_file() or not 0 < sources.stat().st_size <= 2 * 1024 * ARCHIVE.MIB:
        raise ValueError('corresponding-source archive is required (maximum 2 GiB)')
    with sources.open('rb') as stream:
        if stream.read(6) != b'\xfd7zXZ\x00':
            raise ValueError('source materials must be an xz archive')
    verify_sources(sources, metadata, revision)
    shutil.copyfile(sources, output / 'sources.tar.xz')
    return metadata


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--boot', type=Path, required=True)
    parser.add_argument('--base', type=Path, required=True)
    parser.add_argument('--sources', type=Path, required=True)
    parser.add_argument('--reference', required=True, help='ghcr.io/OWNER/PACKAGE:RELEASE')
    parser.add_argument('--revision', required=True, help='full source commit ID')
    parser.add_argument('--oras', default='oras')
    parser.add_argument('--oci-layout', type=Path, help='write a local OCI layout instead of publishing')
    args = parser.parse_args()
    if not re.fullmatch(r'ghcr\.io/[a-z0-9][a-z0-9._/-]*:[A-Za-z0-9_][A-Za-z0-9_.-]*', args.reference):
        parser.error('expected a versioned GHCR reference')
    if not re.fullmatch(r'[0-9a-f]{40}', args.revision):
        parser.error('revision must be a full Git commit ID')
    with tempfile.TemporaryDirectory(prefix='hyper-io-vm-publish-') as temporary:
        staging = Path(temporary)
        metadata = prepare(args.boot, args.base, args.sources, staging, args.revision)
        command = [args.oras, 'push', '--artifact-type', 'application/vnd.hyper.io-vm.v1',
                   '--annotation', 'org.hyper.architecture=aarch64',
                   '--annotation', 'org.hyper.platform=' + metadata['platform'],
                   '--annotation', 'org.hyper.supported-platforms=' + json.dumps(
                       supported_platforms(metadata, (staging / 'kernel.config').read_bytes()),
                       separators=(',', ':')),
                   '--annotation', 'org.hyper.kernel.release=' + metadata['kernel_release'],
                   '--annotation', 'org.opencontainers.image.revision=' + args.revision,
                   '--annotation', 'org.opencontainers.image.source=https://github.com/roolrz/HypeR-io-vm',
                   '--export-manifest', str(staging / 'published.json')]
        if args.oci_layout:
            command += ['--oci-layout-path', str(args.oci_layout.resolve())]
        command += [args.reference, 'Image:application/octet-stream',
                    'initramfs.cpio.gz:application/gzip', 'kernel.config:text/plain',
                    'sources.tar.xz:application/x-xz']
        subprocess.run(command, cwd=staging, check=True)
        digest = hashlib.sha256((staging / 'published.json').read_bytes()).hexdigest()
        print(args.reference.rsplit(':', 1)[0] + '@sha256:' + digest)


if __name__ == '__main__':
    main()
