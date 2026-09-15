#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 roolrz
# SPDX-License-Identifier: Apache-2.0

"""Boot the upstream I/O kernel and exercise vhost-scsi over reserved pages."""

import argparse
import importlib.util
import json
import os
from pathlib import Path
import selectors
import shutil
import subprocess
import tempfile
import time


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("assemble", ROOT / "scripts/assemble-io-vm.py")
ASSEMBLE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ASSEMBLE)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", required=True, type=Path)
    parser.add_argument("--manifest-sha256", required=True)
    parser.add_argument("--test-binary", required=True, type=Path)
    parser.add_argument("--qemu", default="qemu-system-aarch64")
    parser.add_argument("--accel", choices=["tcg", "hvf", "kvm"], default="tcg")
    parser.add_argument("--log", type=Path, default=ROOT / "target/io-vm/acceptance.log")
    args = parser.parse_args()
    args.log.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=args.log.parent, prefix="acceptance-") as temporary:
        work = Path(temporary)
        overlay = work / "overlay"
        (overlay / "usr/bin").mkdir(parents=True)
        shutil.copyfile(ROOT / "tests/io-vm/init", overlay / "init")
        shutil.copyfile(args.test_binary, overlay / "usr/bin/vhost-scsi-test")
        (overlay / "init").chmod(0o755)
        (overlay / "usr/bin/vhost-scsi-test").chmod(0o755)
        result = ASSEMBLE.assemble(argparse.Namespace(
            base=args.base, manifest_sha256=args.manifest_sha256, overlay=overlay,
            output=work / "boot", architecture="aarch64", kernel_limit=32 * ASSEMBLE.MIB,
            compressed_limit=8 * ASSEMBLE.MIB, expanded_limit=32 * ASSEMBLE.MIB))
        disk = work / "test-disk.raw"
        with disk.open("wb") as output:
            output.truncate(8 * ASSEMBLE.MIB)
        dtb = work / "guest.dtb"
        cpu = "host" if args.accel != "tcg" else "cortex-a76"
        hardware = ["-cpu", cpu, "-smp", "2", "-m", "256", "-accel", args.accel,
                    "-drive", f"if=none,id=disk,file={disk},format=raw",
                    "-device", "virtio-scsi-device,id=scsi", "-device", "scsi-hd,drive=disk,bus=scsi.0",
                    "-nographic", "-no-reboot", "-nic", "none"]
        subprocess.run([args.qemu, "-machine", f"virt,gic-version=3,dumpdtb={dtb}", *hardware],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=20)
        source = subprocess.check_output(["dtc", "-I", "dtb", "-O", "dts", str(dtb)],
                                         stderr=subprocess.DEVNULL).decode()
        source += '''
/ {
    reserved-memory {
        #address-cells = <2>;
        #size-cells = <2>;
        ranges;
        hyper_test_memory: memory@4f000000 {
            reg = <0 0x4f000000 0 0x100000>;
        };
    };
    hyper-guest-memory {
        compatible = "hyper,guest-memory-v1";
        memory-region = <&hyper_test_memory>;
    };
};
'''
        subprocess.run(["dtc", "-I", "dts", "-O", "dtb", "-o", str(dtb)], input=source.encode(),
                       check=True, stderr=subprocess.PIPE)
        command = [args.qemu, "-machine", "virt,gic-version=3", *hardware, "-dtb", str(dtb),
                   "-kernel", str(work / "boot" / result["kernel"]),
                   "-initrd", str(work / "boot" / result["initramfs"]),
                   "-append", "console=ttyAMA0 rdinit=/init panic=-1 ignore_loglevel"]
        with args.log.open("wb") as log:
            process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                       stderr=subprocess.STDOUT)
            selector = selectors.DefaultSelector()
            selector.register(process.stdout, selectors.EVENT_READ)
            collected = bytearray()
            deadline = time.monotonic() + 120
            try:
                while time.monotonic() < deadline:
                    for key, _ in selector.select(timeout=1):
                        chunk = os.read(key.fd, 65536)
                        if not chunk:
                            selector.unregister(key.fileobj)
                            continue
                        log.write(chunk)
                        log.flush()
                        collected.extend(chunk)
                    if process.poll() is not None and not selector.get_map():
                        break
                else:
                    raise RuntimeError(f"I/O VM acceptance timed out; see {args.log}")
                if (process.returncode != 0 or b"Kernel panic" in collected or
                        b"Unable to load target_core_" in collected or
                        b"HypeR I/O: module lookup PASS" not in collected or
                        b"HypeR I/O: acceptance complete" not in collected):
                    raise RuntimeError(f"I/O VM acceptance failed; see {args.log}")
                # Check backing storage independently of the frontend readback.
                with disk.open("rb") as source_disk:
                    source_disk.seek(8 * 512)
                    if source_disk.read(512) != bytes((index + 31 * 17) & 255 for index in range(512)):
                        raise RuntimeError("backing disk content differs from the completed write")
            finally:
                selector.close()
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
                process.stdout.close()
        print(json.dumps({"result": "passed", "log": str(args.log)}))


if __name__ == "__main__":
    main()
