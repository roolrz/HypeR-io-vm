#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 roolrz
# SPDX-License-Identifier: Apache-2.0
"""Boot the diskless appliance on GICv2 without any storage device."""
import argparse
import json
from pathlib import Path
import subprocess
import time

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--boot-index', type=Path, required=True)
    parser.add_argument('--log', type=Path, required=True)
    parser.add_argument('--qemu', default='qemu-system-aarch64')
    args = parser.parse_args()
    boot = Path(json.loads(args.boot_index.read_text())['directory'])
    manifest = json.loads((boot / 'boot-artifacts.json').read_text())
    args.log.parent.mkdir(parents=True, exist_ok=True)
    with args.log.open('wb') as log:
        process = subprocess.Popen([
            args.qemu, '-machine', 'virt,gic-version=2', '-accel', 'tcg',
            '-cpu', 'cortex-a76', '-smp', '2', '-m', '128',
            '-nographic', '-no-reboot', '-nic', 'none',
            '-kernel', str(boot / manifest['kernel']),
            '-initrd', str(boot / manifest['initramfs']),
            '-append', 'console=ttyAMA0 rdinit=/init panic=-1 hyper.role=io hyper.mode=bringup'],
            stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 60
            while time.monotonic() < deadline:
                data = args.log.read_text(errors='replace')
                if 'Kernel panic' in data or process.poll() is not None:
                    raise RuntimeError(f'bring-up terminated: {args.log}')
                if 'HypeR I/O: bring-up ready; no devices assigned, storage service disabled' in data:
                    if 'backend service ready' in data:
                        raise RuntimeError('diskless bring-up advertised a storage service')
                    print('Diskless appliance GICv2 boot: PASS')
                    return
                time.sleep(0.1)
            raise RuntimeError(f'bring-up timed out: {args.log}')
        finally:
            if process.poll() is None:
                process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()

if __name__ == '__main__':
    main()
