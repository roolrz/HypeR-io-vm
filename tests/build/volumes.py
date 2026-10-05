# SPDX-FileCopyrightText: 2026 roolrz
# SPDX-License-Identifier: Apache-2.0
"""Exercise the exact C manifest parser without any device or mount operations."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[2]
GOOD = 'hyper.volumes.v1\nconfig 12345678-1234-1234-1234-123456789abc 2097152 hyper hyper-config\nvm-a 12345678-1234-1234-1234-123456789abd 32768 vm-a hyper-vm-a\n'
class Volumes(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.binary = Path(cls.temp.name) / 'parser'
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-DHYPER_VOLUMES_PARSE_TEST', '-I', str(ROOT / 'include'), str(ROOT / 'service/hyper-volumes.c'),
                        '-o', str(cls.binary)], check=True)
    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()
    def parse(self, text):
        path = Path(self.temp.name) / 'volumes.conf'
        path.write_text(text)
        return subprocess.run([str(self.binary), str(path)]).returncode == 0
    def test_valid(self):
        self.assertTrue(self.parse(GOOD))
    def test_rejects_invalid(self):
        cases = [GOOD.replace('v1', 'v2'), GOOD.replace('2097152', '0'),
                 GOOD.replace('2097152', '-1'), GOOD.replace('2097152', '18446744073709551616'),
                 GOOD.replace('hyper-config', '../config'), GOOD.replace(' hyper ', ' vm-a '),
                 GOOD.replace('789abd', '789abc'), GOOD + GOOD.splitlines()[2] + '\n',
                 GOOD.rstrip(), 'hyper.volumes.v1\n', GOOD.replace(' vm-a hyper-vm-a', ' hyper hyper-vm-a'),
                 GOOD.replace('789abc', '789abg'), GOOD.replace('2097152', '512 extra')]
        for text in cases:
            with self.subTest(text=text): self.assertFalse(self.parse(text))
    def test_client_authorization(self):
        manifest = Path(self.temp.name) / 'volumes.conf'
        manifest.write_text(GOOD)
        clients = Path(self.temp.name) / 'clients.conf'
        for content, good in [
            ('hyper.clients.v2\n0 config - -\n7 vm-a - -\n', True),
            ('hyper.clients.v2\n0 config - -\n7 missing - -\n', False),
            ('hyper.clients.v2\n0 config - -\n0 vm-a - -\n', False),
            ('hyper.clients.v2\n0 config - -\n1 vm-a - -\n2 vm-a - -\n', False),
            ('hyper.clients.v2\n1 config - -\n', False),
            ('hyper.clients.v2\n1 vm-a - -\n', False),
            ('hyper.clients.v2\n0 config - -\n128 vm-a - -\n', False),
            ('hyper.clients.v2\n0 config - -\n', True),
            ('hyper.clients.v2\n0 config - -\n1 - default 02:48:59:00:00:01\n', True),
            ('hyper.clients.v2\n0 config - -\n1 vm-a default 02:48:59:00:00:01\n', True),
            ('hyper.clients.v2\n0 config default 02:48:59:00:00:01\n', False),
            ('hyper.clients.v2\n0 config - -\n1 - - -\n', False),
            ('hyper.clients.v2\n0 config - -\n1 - default 03:48:59:00:00:01\n', False),
            ('hyper.clients.v2\n0 config - -\n1 - - 02:48:59:00:00:01\n', False),
            ('hyper.clients.v2\n0 config - -\n1 - default 02:48:59:00:00:01\n2 - default 02:48:59:00:00:01\n', False),
        ]:
            clients.write_text(content)
            result = subprocess.run([str(self.binary), str(manifest), str(clients)], capture_output=True)
            self.assertEqual(result.returncode == 0, good)
            if not good: self.assertEqual(result.stdout, b'')
    def test_network_manifest(self):
        source = Path(self.temp.name) / 'networks.c'
        source.write_text('#include "hyper_network.h"\n'
                          'int main(int argc, char **argv) { struct hyper_network n[HYPER_MAX_NETWORKS]; '
                          'unsigned count; return argc != 2 || hyper_networks_read(argv[1], n, &count); }\n')
        binary = Path(self.temp.name) / 'networks'
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-I', str(ROOT / 'include'), str(source), str(ROOT / 'service/network-config.c'),
                        '-o', str(binary)], check=True)
        manifest = Path(self.temp.name) / 'networks.conf'
        for content, good in [
            ('hyper.networks.v1\n', True),
            ('hyper.networks.v1\ndefault hbr0 eth0\n', True),
            ('hyper.networks.v1\ndefault hbr0 hbr0\n', False),
            ('hyper.networks.v1\ndefault ../x eth0\n', False),
            ('hyper.networks.v1\ndefault hbr0 eth0 extra\n', False),
            ('hyper.networks.v1\ndefault hbr0 eth0', False),
            ('hyper.networks.v1\ndefault hbr0 eth0\nother hbr0 eth1\n', False),
            ('hyper.networks.v1\ndefault hbr0 eth0\nother hbr1 eth0\n', False),
            ('hyper.networks.v1\ndefault hbr0 eth0\nother eth0 eth1\n', False),
        ]:
            with self.subTest(content=content):
                manifest.write_text(content)
                self.assertEqual(subprocess.run([str(binary), str(manifest)]).returncode == 0, good)

    def test_session_state_machine(self):
        binary = Path(self.temp.name) / 'session'
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-I', str(ROOT / 'include'), str(ROOT / 'tests/build/session.c'),
                        '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)

if __name__ == '__main__': unittest.main()
