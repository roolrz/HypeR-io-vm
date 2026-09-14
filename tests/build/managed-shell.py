# SPDX-FileCopyrightText: 2026 roolrz
# SPDX-License-Identifier: Apache-2.0
"""Execute the actual managed LIO setup using the packaged minimal shell.

A temporary regular directory stands in for configfs; real block/DM behavior
is covered separately by the full-system integration run.
"""
import pathlib
import subprocess
import sys
import tempfile

root = pathlib.Path(__file__).resolve().parents[2]
busybox = pathlib.Path(sys.argv[1]).resolve()
source = (root / 'rootfs/init').read_text()
setup = source.split('    index=0\n', 1)[1].split('\nelse\n', 1)[0]
with tempfile.TemporaryDirectory() as tmp:
    tmp = pathlib.Path(tmp)
    manifest = tmp / 'volumes.conf'
    manifest.write_text('hyper.volumes.v1\nconfig uuid 100 hyper hyper-config\nvm uuid2 200 vm hyper-vm\n')
    script = 'set -eu\nindex=0\n' + setup.replace('/sys/kernel/config', str(tmp / 'configfs')).replace('/etc/hyper-volumes.conf', str(manifest))
    subprocess.run(['qemu-aarch64', str(busybox), 'sh', '-c', script], check=True)
    for index, mapper in enumerate(('hyper-config', 'hyper-vm'), 1):
        core = tmp / 'configfs/target/core/iblock_0' / mapper
        target = tmp / f'configfs/target/vhost/naa.5001405{index:09x}/tpgt_1'
        assert (core / 'control').read_text() == f'udev_path=/dev/mapper/{mapper}\n'
        assert (core / 'enable').read_text() == '1\n'
        assert (target / 'nexus').read_text() == 'naa.5001405000000002\n'
        assert (target / 'lun/lun_0/disk').resolve() == core
print('packaged BusyBox managed volume setup passed')
