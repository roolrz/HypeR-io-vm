#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 roolrz
# SPDX-License-Identifier: Apache-2.0
"""Boot the complete appliance's business role against a disposable QEMU disk."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--boot', type=Path, required=True)
    parser.add_argument('--qemu', default='qemu-system-aarch64')
    parser.add_argument('--log', type=Path, required=True)
    parser.add_argument('--reset', action='store_true')
    args = parser.parse_args()
    manifest = json.loads((args.boot / 'boot-artifacts.json').read_text())
    args.log.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as temporary:
        disk = Path(temporary) / 'disk.raw'
        with disk.open('wb') as output:
            output.truncate(8 * 1024 * 1024)
        command = [args.qemu, '-machine', 'virt,gic-version=3', '-accel', 'tcg',
                   '-cpu', 'cortex-a76', '-smp', '2', '-m', '128', '-nographic',
                   '-no-reboot', '-nic', 'none', '-drive', f'if=none,id=disk,file={disk},format=raw',
                   '-device', 'virtio-scsi-device,id=scsi', '-device', 'scsi-hd,drive=disk,bus=scsi.0',
                   '-kernel', str(args.boot / manifest['kernel']),
                   '-initrd', str(args.boot / manifest['initramfs']),
                   '-append', 'console=ttyAMA0 rdinit=/init panic=-1 hyper.role=business'
                   + (' hyper.test=reset' if args.reset else '')]
        with args.log.open('wb') as log:
            result = subprocess.run(command, stdin=subprocess.DEVNULL, stdout=log,
                                    stderr=subprocess.STDOUT, timeout=60)
        log = args.log.read_text(errors='replace')
        if result.returncode or 'HypeR business disk: acceptance complete' not in log or 'Kernel panic' in log:
            raise RuntimeError(f'business appliance failed: {args.log}')
        if args.reset and 'HypeR business disk: reset/rebind PASS' not in log:
            raise RuntimeError(f'business reset/rebind failed: {args.log}')
        with disk.open('rb') as source:
            source.seek(4096)
            if source.read(512) != bytes((index + 31 * 17) & 255 for index in range(512)):
                raise RuntimeError('business appliance backing disk mismatch')
    print('Standalone business appliance: PASS with independently checked backing disk')


if __name__ == '__main__':
    main()
