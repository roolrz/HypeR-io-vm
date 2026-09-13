#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 roolrz
# SPDX-License-Identifier: Apache-2.0

"""Verify immutable I/O VM artifacts and assemble a deterministic Linux ramdisk."""

import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import stat
import tempfile
import zlib


MIB = 1024 * 1024
MAX_ENTRIES = 16384
KINDS = {stat.S_IFREG, stat.S_IFDIR, stat.S_IFLNK, stat.S_IFCHR, stat.S_IFBLK}


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def bounded_read(path, limit):
    with Path(path).open("rb") as source:
        data = source.read(limit + 1)
    if len(data) > limit:
        raise ValueError(f"{path}: exceeds {limit}-byte budget")
    return data


def archive_name(name):
    # Never extract archives onto the host. Still enforce canonical paths so
    # Linux cannot resolve an overlay through a base archive's symlink parent.
    if not name or "\0" in name or name.startswith("/"):
        raise ValueError(f"invalid archive path: {name!r}")
    if any(part in ("", ".", "..") for part in name.split("/")):
        raise ValueError(f"noncanonical archive path: {name!r}")
    if len(name.encode()) > 4095:
        raise ValueError("archive path is too long")
    return name


def inflate(data, limit):
    stream = zlib.decompressobj(16 + zlib.MAX_WBITS)
    expanded = stream.decompress(data, limit + 1)
    if len(expanded) > limit or stream.unconsumed_tail:
        raise ValueError("expanded base initramfs exceeds budget")
    if not stream.eof or stream.unused_data:
        raise ValueError("base must contain exactly one complete gzip stream")
    return expanded


def parse_newc(data):
    entries = {}
    offset = 0
    while offset + 110 <= len(data):
        header = data[offset : offset + 110]
        if header[:6] != b"070701":
            raise ValueError("base archive must use newc without CRC")
        try:
            fields = [int(header[i : i + 8], 16) for i in range(6, 110, 8)]
        except ValueError as error:
            raise ValueError("invalid newc header") from error
        _, mode, uid, gid, links, _, size, _, _, major, minor, namesize, check = fields
        start = offset + 110
        end = start + namesize
        if namesize < 1 or namesize > 4096 or end > len(data):
            raise ValueError("invalid newc name extent")
        name_bytes = data[start:end]
        if name_bytes[-1:] != b"\0" or b"\0" in name_bytes[:-1]:
            raise ValueError("invalid newc name terminator")
        name = name_bytes[:-1].decode("utf-8")
        payload = (end + 3) & ~3
        offset = (payload + size + 3) & ~3
        if offset > len(data) or check:
            raise ValueError("invalid newc payload extent or checksum field")
        contents = data[payload : payload + size]
        if name == "TRAILER!!!":
            if size or any(data[offset:]):
                raise ValueError("unexpected data after newc trailer")
            validate_tree(entries)
            return entries
        name = archive_name(name)
        kind = stat.S_IFMT(mode)
        if kind not in KINDS or links < 1:
            raise ValueError(f"unsupported archive entry: {name}")
        if kind == stat.S_IFREG and links != 1:
            raise ValueError("base contract uses independent files and symlinks, not hardlinks")
        if kind not in (stat.S_IFREG, stat.S_IFLNK) and size:
            raise ValueError(f"unexpected payload for {name}")
        if kind == stat.S_IFLNK and (not contents or b"\0" in contents):
            raise ValueError(f"invalid symlink: {name}")
        if name in entries or len(entries) >= MAX_ENTRIES:
            raise ValueError("duplicate archive path or too many entries")
        entries[name] = (mode, uid, gid, major, minor, contents)
    raise ValueError("missing newc trailer")


def validate_tree(entries):
    for name in entries:
        for parent in PurePosixPath(name).parents:
            if str(parent) == ".":
                continue
            entry = entries.get(str(parent))
            if entry is None or not stat.S_ISDIR(entry[0]):
                raise ValueError(f"{name}: parent {parent} is not an explicit directory")


def overlay_entries(root, limit):
    if not root.is_dir() or root.is_symlink():
        raise ValueError("overlay root must be a directory")
    entries = {}
    remaining = limit
    for directory, directories, files in os.walk(root, followlinks=False):
        for basename in sorted(directories + files):
            path = Path(directory) / basename
            name = archive_name(path.relative_to(root).as_posix())
            info = path.lstat()
            kind = stat.S_IFMT(info.st_mode)
            if kind == stat.S_IFREG:
                contents = bounded_read(path, remaining)
            elif kind == stat.S_IFLNK:
                contents = os.readlink(path).encode()
            elif kind == stat.S_IFDIR:
                contents = b""
            else:
                raise ValueError(f"unsupported overlay entry: {name}")
            remaining -= len(contents)
            if remaining < 0 or len(entries) >= MAX_ENTRIES:
                raise ValueError("overlay exceeds budget")
            entries[name] = (info.st_mode, 0, 0, 0, 0, contents)
    return entries


def encode_newc(entries, limit):
    validate_tree(entries)
    result = bytearray()
    ordered = sorted(entries.items(), key=lambda item: (item[0].count("/"), item[0]))
    ordered.append(("TRAILER!!!", (0, 0, 0, 0, 0, b"")))
    for inode, (name, entry) in enumerate(ordered, start=1):
        mode, uid, gid, major, minor, contents = entry
        encoded = name.encode() + b"\0"
        values = (inode, mode, uid, gid, 1, 0, len(contents), 0, 0,
                  major, minor, len(encoded), 0)
        if any(value < 0 or value > 0xFFFFFFFF for value in values):
            raise ValueError("newc field exceeds 32 bits")
        required = ((len(result) + 110 + len(encoded) + 3) & ~3) + len(contents)
        if ((required + 3) & ~3) > limit:
            raise ValueError("assembled initramfs exceeds expanded budget")
        result.extend(b"070701" + b"".join(f"{value:08x}".encode() for value in values))
        result.extend(encoded)
        result.extend(b"\0" * (-len(result) % 4))
        result.extend(contents)
        result.extend(b"\0" * (-len(result) % 4))
    return bytes(result)


def publish(path, contents):
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.is_file() and path.stat().st_size == len(contents) and bounded_read(path, len(contents)) == contents:
        return
    with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as output:
        temporary = Path(output.name)
        try:
            output.write(contents)
            output.flush()
            os.fsync(output.fileno())
            output.close()
            temporary.replace(path)
        finally:
            temporary.unlink(missing_ok=True)


def assemble(args):
    manifest_bytes = bounded_read(args.base / "manifest.json", MIB)
    if sha256(manifest_bytes) != args.manifest_sha256:
        raise ValueError("base manifest does not match the pinned SHA-256")
    manifest = json.loads(manifest_bytes)
    if manifest.get("format") != 1 or manifest.get("architecture") != args.architecture:
        raise ValueError("unsupported base format or architecture")
    verified = {}
    for name, limit in (("Image", args.kernel_limit), ("base.cpio.gz", args.compressed_limit)):
        data = bounded_read(args.base / name, limit)
        expected = manifest["files"][name]
        if expected.get("size") != len(data) or expected.get("sha256") != sha256(data):
            raise ValueError(f"base artifact verification failed: {name}")
        verified[name] = data
    if verified["Image"][56:60] != b"ARM\x64":
        raise ValueError("base kernel is not an AArch64 Linux Image")
    base = parse_newc(inflate(verified["base.cpio.gz"], args.expanded_limit))
    overlay = overlay_entries(args.overlay, args.expanded_limit)
    # Replacing directories by files (or the reverse) leaves ambiguous subtree
    # ownership. Require the producer to change its base layout explicitly.
    for name, entry in overlay.items():
        if name in base and stat.S_ISDIR(base[name][0]) != stat.S_ISDIR(entry[0]):
            raise ValueError(f"overlay changes directory type: {name}")
    base.update(overlay)
    init = base.get("init")
    if init is None or not stat.S_ISREG(init[0]) or not init[0] & 0o111:
        raise ValueError("HypeR overlay must provide an executable regular /init")
    if "init" not in overlay:
        raise ValueError("/init launch policy must come from the HypeR overlay")
    expanded = encode_newc(base, args.expanded_limit)
    # GzipFile produces a platform-independent header with no filename/mtime.
    import io
    compressed = io.BytesIO()
    with gzip.GzipFile(fileobj=compressed, mode="wb", filename="", mtime=0, compresslevel=9) as stream:
        stream.write(expanded)
    ramdisk = compressed.getvalue()
    if len(ramdisk) > args.compressed_limit:
        raise ValueError("assembled initramfs exceeds compressed budget")
    # Publish a content-addressed generation; the manifest is the commit point.
    # Readers never observe a new kernel paired with an old ramdisk.
    image_name = f"Image-{sha256(verified['Image'])}"
    ramdisk_name = f"initramfs-{sha256(ramdisk)}.cpio.gz"
    publish(args.output / image_name, verified["Image"])
    publish(args.output / ramdisk_name, ramdisk)
    result = {
        "format": 1,
        "architecture": args.architecture,
        "base_manifest_sha256": args.manifest_sha256,
        "kernel": image_name,
        "initramfs": ramdisk_name,
        "initramfs_expanded_size": len(expanded),
        "initramfs_compressed_size": len(ramdisk),
    }
    publish(args.output / "boot-artifacts.json", (json.dumps(result, sort_keys=True, indent=2) + "\n").encode())
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", required=True, type=Path)
    parser.add_argument("--manifest-sha256", required=True)
    parser.add_argument("--overlay", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--architecture", choices=["aarch64"], default="aarch64")
    parser.add_argument("--kernel-limit", type=int, default=32 * MIB)
    parser.add_argument("--compressed-limit", type=int, default=8 * MIB)
    parser.add_argument("--expanded-limit", type=int, default=32 * MIB)
    args = parser.parse_args()
    if min(args.kernel_limit, args.compressed_limit, args.expanded_limit) <= 0:
        parser.error("size budgets must be positive")
    try:
        result = assemble(args)
    except (OSError, ValueError, KeyError, TypeError, zlib.error) as error:
        parser.exit(1, f"assemble-io-vm: {error}\n")
    print(json.dumps(result, sort_keys=True))


if __name__ == "__main__":
    main()
