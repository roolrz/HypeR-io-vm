#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 roolrz
# SPDX-License-Identifier: Apache-2.0

"""Build a pinned, unmodified upstream Linux kernel and minimal base initramfs."""

import argparse
import gzip
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tarfile
import tempfile
import urllib.request


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("io_archive", ROOT / "scripts/assemble-io-vm.py")
ARCHIVE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ARCHIVE)


def digest(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def fetch(component, cache):
    destination = cache / component["url"].rsplit("/", 1)[1]
    if destination.is_file() and digest(destination) == component["sha256"]:
        return destination
    cache.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=cache, delete=False) as output:
        temporary = Path(output.name)
        try:
            with urllib.request.urlopen(component["url"], timeout=60) as response:
                shutil.copyfileobj(response, output)
            output.close()
            if digest(temporary) != component["sha256"]:
                raise ValueError(f"source checksum mismatch: {component['url']}")
            temporary.replace(destination)
        finally:
            temporary.unlink(missing_ok=True)
    return destination


def unpack(archive, component, sources, name):
    destination = sources / f"{name}-{component['version']}"
    stamp = destination / ".hyper-source-sha256"
    if destination.exists():
        if stamp.read_text().strip() != component["sha256"]:
            raise ValueError(f"source identity changed at {destination}; select a new output directory")
        return destination
    sources.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=sources) as temporary:
        staging = Path(temporary)
        with tarfile.open(archive) as source:
            source.extractall(staging, filter="data")
        expanded = staging / destination.name
        (expanded / ".hyper-source-sha256").write_text(component["sha256"] + "\n")
        expanded.replace(destination)
    return destination


def run(command, **kwargs):
    print("+", " ".join(str(part) for part in command), flush=True)
    subprocess.run(command, check=True, **kwargs)


def config_values(data):
    result = {}
    for line in data.splitlines():
        if line.startswith("CONFIG_") and "=" in line:
            key, value = line.split("=", 1)
            result[key] = value
        elif line.startswith("# CONFIG_") and line.endswith(" is not set"):
            result[line[2:-11]] = "n"
    return result


def check_config(requested, actual):
    selected = config_values(actual.read_text())
    mismatches = [f"{key}: wanted {value}, got {selected.get(key, 'n')}"
                  for key, value in config_values(requested).items()
                  if selected.get(key, "n") != value]
    if mismatches:
        raise ValueError("configuration requirements were dropped:\n" + "\n".join(mismatches))


def source_bundle(output, build, linux_output, busybox_output, module_checkout):
    """Retain exact upstream archives and all local build inputs beside binaries."""
    paths = []
    for directory in ("configs", "include", "modules", "service", "rootfs", "tools", "scripts", "tests", "LICENSES"):
        origin = module_checkout if directory in ("include", "modules") else ROOT
        paths.extend((path, "hyper-io-vm/" + str(path.relative_to(origin)))
                     for path in sorted((origin / directory).rglob("*"))
                     if path.is_file() and "__pycache__" not in path.parts)
    for name in ("Makefile", "sources.lock.json", "README.md", "LICENSE"):
        paths.append((ROOT / name, "hyper-io-vm/" + name))
    paths.extend((path, "upstream/" + path.name) for path in sorted((output / "downloads").iterdir()) if path.is_file())
    paths.extend(((linux_output / ".config", "resolved/linux.config"),
                  (busybox_output / ".config", "resolved/busybox.config"),
                  (build / "compiler.txt", "resolved/compiler.txt")))
    temporary = build / "corresponding-sources.tar.xz"
    with tarfile.open(temporary, "w:xz", preset=0) as archive:
        for path, name in paths:
            info = archive.gettarinfo(str(path), arcname=name)
            info.uid = info.gid = info.mtime = 0
            info.uname = info.gname = ""
            with path.open("rb") as data:
                archive.addfile(info, data)
    destination = output / "source-artifacts" / (digest(temporary) + ".tar.xz")
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary.replace(destination)
    return destination


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "target/io-vm")
    parser.add_argument("--platform", choices=["qemu", "rpi5"], default="qemu")
    parser.add_argument("--cross-compile", default=os.environ.get("CROSS_COMPILE", ""))
    parser.add_argument("--jobs", type=int, default=min(os.cpu_count() or 1, 8))
    parser.add_argument("--module-source", type=Path, default=ROOT,
                        help="module checkout (defaults to this repository)")
    parser.add_argument("--fetch-only", action="store_true")
    args = parser.parse_args()
    lock = json.loads((ROOT / "sources.lock.json").read_text())
    if lock["format"] != 1 or not lock["linux"]["version"].startswith(lock["linux"]["series"] + "."):
        parser.error("invalid pinned Linux LTS identity")
    output = args.output.resolve()
    sources = {}
    for name in ("linux", "busybox"):
        archive = fetch(lock[name], output / "downloads")
        sources[name] = unpack(archive, lock[name], output / "sources", name)
    if args.fetch_only:
        return
    if platform.system() != "Linux":
        parser.error("kernel and module builds require a Linux build host; use --fetch-only on other hosts")
    if args.jobs <= 0:
        parser.error("--jobs must be positive")
    build = output / "build" / args.platform
    linux_output, busybox_output = build / "linux", build / "busybox"
    build.mkdir(parents=True, exist_ok=True)
    compiler_identity = subprocess.check_output([args.cross_compile + "gcc", "--version"], text=True)
    ARCHIVE.publish(build / "compiler.txt", compiler_identity.encode())
    linux_output.mkdir(parents=True, exist_ok=True)
    busybox_output.mkdir(parents=True, exist_ok=True)
    environment = dict(os.environ, KBUILD_BUILD_USER="hyper", KBUILD_BUILD_HOST="builder",
                       KBUILD_BUILD_TIMESTAMP="Thu Jan  1 00:00:00 UTC 1970", KBUILD_BUILD_VERSION="1",
                       SOURCE_DATE_EPOCH="0", LC_ALL="C")
    linux_make = ["make", "-C", str(sources["linux"]), f"O={linux_output}", "ARCH=arm64",
                  f"CROSS_COMPILE={args.cross_compile}"]
    requested = (ROOT / "configs/linux-aarch64.config").read_text()
    if args.platform == "rpi5":
        requested += "\n" + (ROOT / "configs/linux-rpi5.config").read_text()
    profile = build / "linux.requested.config"
    ARCHIVE.publish(profile, requested.encode())
    state = linux_output / ".hyper-config-input"
    identity = hashlib.sha256((requested + args.cross_compile + compiler_identity + lock["linux"]["sha256"]).encode()).hexdigest()
    if not state.exists() or state.read_text() != identity:
        run(linux_make + [f"KCONFIG_ALLCONFIG={profile}", "allnoconfig"], env=environment)
        check_config(requested, linux_output / ".config")
        state.write_text(identity)
    check_config(requested, linux_output / ".config")
    run(linux_make + [f"-j{args.jobs}", "Image", "modules"], env=environment)
    release = subprocess.check_output(linux_make + ["-s", "--no-print-directory", "kernelrelease"],
                                      env=environment, text=True).strip()
    source_revision = subprocess.check_output(["git", "-C", str(ROOT), "rev-parse", "HEAD"], text=True).strip()
    source_dirty = bool(subprocess.check_output(["git", "-C", str(ROOT), "status", "--porcelain"], text=True))
    checkout = args.module_source.resolve() if args.module_source else ROOT
    if digest(checkout / "include/hyper_io.h") != digest(ROOT / "include/hyper_io.h"):
        raise ValueError("module checkout and service bridge headers differ")
    module_paths = ["include/hyper_io.h", "modules/guest-memory/Makefile",
                    "modules/guest-memory/hyper_guest_memory.c", "modules/io-bridge/Makefile",
                    "modules/io-bridge/hyper_io_bridge.c"]
    input_hashes = {name: digest(checkout / name) for name in module_paths}
    source_dirty |= any(input_hashes[name] != digest(ROOT / name) for name in module_paths)
    for name in ("Makefile", "tools/build.py", "scripts/assemble-io-vm.py", "rootfs/init",
                 "service/hyper-io-service.c", "service/hyper-volumes.c", "service/hyper-io-supervisor.c", "include/hyper_io_session.h", "tests/io-vm/business-disk.c", "sources.lock.json",
                 "configs/linux-aarch64.config", "configs/busybox.config", "LICENSE", "LICENSES/GPL-2.0-only.txt"):
        input_hashes[name] = digest(ROOT / name)
    if args.platform == "rpi5":
        input_hashes["configs/linux-rpi5.config"] = digest(ROOT / "configs/linux-rpi5.config")
    modules = []
    module_identity = None
    if args.module_source:
        checkout = args.module_source.resolve()
        source_files = [checkout / "include/hyper_io.h"]
        for name, basename in (("guest-memory", "hyper_guest_memory"), ("io-bridge", "hyper_io_bridge")):
            module_source = checkout / "modules" / name
            module_output = build / "modules" / name
            module_output.mkdir(parents=True, exist_ok=True)
            source_files.extend([module_source / "Makefile", module_source / f"{basename}.c"])
            run(linux_make + [f"M={module_source}", f"MO={module_output}", f"-j{args.jobs}", "modules"], env=environment)
            module = module_output / f"{basename}.ko"
            vermagic = subprocess.check_output(["modinfo", "-F", "vermagic", str(module)], text=True).split()
            if not vermagic or vermagic[0] != release:
                raise ValueError("external module does not match this kernel release")
            modules.append(module)
        module_identity = hashlib.sha256("".join(digest(path) for path in source_files).encode()).hexdigest()
    binaries = []
    for source, name in (("service/hyper-io-service.c", "hyper-io-service"),
                         ("service/hyper-volumes.c", "hyper-volumes"),
                         ("service/hyper-io-supervisor.c", "hyper-io-supervisor"),
                         ("tests/io-vm/business-disk.c", "hyper-disk-test")):
        binary = build / name
        run([args.cross_compile + "gcc", "-static", "-O2", "-Wall", "-Wextra", "-Werror",
             "-I", str(ROOT / "include"), str(ROOT / source), "-o", str(binary)], env=environment)
        binaries.append(binary)

    busybox_make = ["make", "-C", str(sources["busybox"]), f"O={busybox_output}", "ARCH=arm64",
                    f"CROSS_COMPILE={args.cross_compile}"]
    requested = (ROOT / "configs/busybox.config").read_text()
    state = busybox_output / ".hyper-config-input"
    identity = hashlib.sha256((requested + args.cross_compile + compiler_identity + lock["busybox"]["sha256"]).encode()).hexdigest()
    if not state.exists() or state.read_text() != identity:
        run(busybox_make + ["allnoconfig"], env=environment, stdout=subprocess.DEVNULL)
        selected = config_values((busybox_output / ".config").read_text())
        selected.update(config_values(requested))
        (busybox_output / ".config").write_text("".join(f"{key}={value}\n" for key, value in selected.items()))
        run(busybox_make + ["oldconfig"], env=environment, input="\n" * 16384, text=True,
            stdout=subprocess.DEVNULL)
        check_config(requested, busybox_output / ".config")
        state.write_text(identity)
    run(busybox_make + [f"-j{args.jobs}"], env=environment)

    with tempfile.TemporaryDirectory(dir=build) as temporary:
        staging = Path(temporary)
        rootfs = staging / "rootfs"
        rootfs.mkdir()
        run(busybox_make + [f"CONFIG_PREFIX={rootfs}", "install"], env=environment,
            stdout=subprocess.DEVNULL)
        for name in ("dev", "proc", "sys", "run", "tmp", "etc", f"lib/modules/{release}"):
            (rootfs / name).mkdir(parents=True, exist_ok=True)
        for module in modules:
            shutil.copyfile(module, rootfs / "lib/modules" / release / module.name)
        (rootfs / "usr/bin").mkdir(parents=True, exist_ok=True)
        for binary in binaries:
            shutil.copyfile(binary, rootfs / "usr/bin" / binary.name)
            (rootfs / "usr/bin" / binary.name).chmod(0o755)
        # /init belongs to the product overlay. BusyBox's optional linuxrc
        # symlink must not silently select an alternative boot policy.
        (rootfs / "linuxrc").unlink(missing_ok=True)
        entries = ARCHIVE.overlay_entries(rootfs, 32 * ARCHIVE.MIB)
        import stat
        entries["dev/console"] = (stat.S_IFCHR | 0o600, 0, 0, 5, 1, b"")
        expanded = ARCHIVE.encode_newc(entries, 32 * ARCHIVE.MIB)
        compressed = io.BytesIO()
        with gzip.GzipFile(fileobj=compressed, mode="wb", filename="", mtime=0, compresslevel=9) as stream:
            stream.write(expanded)
        image = ARCHIVE.bounded_read(linux_output / "arch/arm64/boot/Image", 32 * ARCHIVE.MIB)
        base = compressed.getvalue()
        if len(base) > 8 * ARCHIVE.MIB:
            raise ValueError("base ramdisk exceeds compressed size budget")
        manifest = {"format": 1, "architecture": "aarch64", "platform": args.platform,
                    "kernel_release": release, "source_lock": lock,
                    "source_revision": source_revision, "source_dirty": source_dirty,
                    "source_files": input_hashes,
                    "external_module_source_sha256": module_identity,
                    "kernel_config_sha256": digest(linux_output / ".config"),
                    "busybox_config_sha256": digest(busybox_output / ".config"),
                    "compiler_sha256": digest(build / "compiler.txt"),
                    "files": {"Image": {"size": len(image), "sha256": ARCHIVE.sha256(image)},
                              "base.cpio.gz": {"size": len(base), "sha256": ARCHIVE.sha256(base)}}}
        manifest_bytes = (json.dumps(manifest, sort_keys=True, indent=2) + "\n").encode()
        artifacts = output / "artifacts" / ARCHIVE.sha256(manifest_bytes)
        ARCHIVE.publish(artifacts / "Image", image)
        ARCHIVE.publish(artifacts / "base.cpio.gz", base)
        ARCHIVE.publish(artifacts / "kernel.config", (linux_output / ".config").read_bytes())
        ARCHIVE.publish(artifacts / "manifest.json", manifest_bytes)
        ARCHIVE.publish(output / f"{args.platform}-base.json", (json.dumps({
            "directory": str(artifacts), "manifest_sha256": ARCHIVE.sha256(manifest_bytes)
        }, sort_keys=True, indent=2) + "\n").encode())
        boot = output / "boot" / ARCHIVE.sha256(manifest_bytes)
        ARCHIVE.assemble(argparse.Namespace(
            base=artifacts, manifest_sha256=ARCHIVE.sha256(manifest_bytes),
            overlay=ROOT / "rootfs", output=boot, architecture="aarch64",
            kernel_limit=32 * ARCHIVE.MIB, compressed_limit=8 * ARCHIVE.MIB,
            expanded_limit=32 * ARCHIVE.MIB))
        ARCHIVE.publish(output / f"{args.platform}-boot.json", (json.dumps({"directory": str(boot)},
                        sort_keys=True, indent=2) + "\n").encode())
        bundle = source_bundle(output, build, linux_output, busybox_output, checkout)
        print(f"Corresponding sources: {bundle}")
        print(f"Base artifacts: {artifacts}\nManifest SHA-256: {ARCHIVE.sha256(manifest_bytes)}")


if __name__ == "__main__":
    main()
