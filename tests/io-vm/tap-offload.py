#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 roolrz
# SPDX-License-Identifier: Apache-2.0
"""Check IPv4/IPv6 TAP checksum and software segmentation on the pinned kernel."""
import argparse
import importlib.util
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('assemble', ROOT / 'scripts/assemble-io-vm.py')
ASSEMBLE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ASSEMBLE)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base', type=Path, required=True)
    parser.add_argument('--manifest-sha256', required=True)
    parser.add_argument('--test-binary', type=Path, required=True)
    parser.add_argument('--log', type=Path, required=True)
    parser.add_argument('--qemu', default='qemu-system-aarch64')
    args = parser.parse_args()
    args.log.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=args.log.parent, prefix='tap-offload-') as temporary:
        work = Path(temporary)
        overlay = work / 'overlay'
        (overlay / 'usr/bin').mkdir(parents=True)
        binary = overlay / 'usr/bin/tap-offload-test'
        shutil.copyfile(args.test_binary, binary)
        binary.chmod(0o755)
        (overlay / 'init').write_text('''#!/bin/sh
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
if /usr/bin/tap-offload-test; then echo TAP-ACCEPTANCE-PASS; fi
poweroff -f
''')
        (overlay / 'init').chmod(0o755)
        result = ASSEMBLE.assemble(argparse.Namespace(
            base=args.base, manifest_sha256=args.manifest_sha256, overlay=overlay,
            output=work / 'boot', architecture='aarch64', kernel_limit=32 * ASSEMBLE.MIB,
            compressed_limit=8 * ASSEMBLE.MIB, expanded_limit=32 * ASSEMBLE.MIB))
        command = [args.qemu, '-machine', 'virt,gic-version=3', '-accel', 'tcg',
                   '-cpu', 'cortex-a76', '-smp', '2', '-m', '128', '-nodefaults',
                   '-display', 'none', '-serial', 'stdio', '-monitor', 'none', '-no-reboot',
                   '-kernel', str(work / 'boot' / result['kernel']),
                   '-initrd', str(work / 'boot' / result['initramfs']),
                   '-append', 'console=ttyAMA0 rdinit=/init panic=-1']
        with args.log.open('wb') as log:
            # subprocess.run kills and waits for the owned QEMU on timeout.
            outcome = subprocess.run(command, stdin=subprocess.DEVNULL, stdout=log,
                                     stderr=subprocess.STDOUT, timeout=120, check=False)
        output = args.log.read_bytes()
        if (outcome.returncode or b'Kernel panic' in output or
                b'TAP-OFFLOAD-PASS' not in output or b'TAP-ACCEPTANCE-PASS' not in output):
            raise RuntimeError(f'TAP offload acceptance failed: {args.log}')
    print(f'TAP IPv4/IPv6 checksum and segmentation: PASS ({args.log})')


if __name__ == '__main__':
    main()
