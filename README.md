<!-- SPDX-FileCopyrightText: 2026 roolrz -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# HypeR I/O VM

A Linux appliance under development for HypeR's virtio-scsi storage backend. This repository
owns Linux builds, external drivers, Linux-side services, complete initramfs
assembly, tests, and GHCR package publication. HypeR owns Native apps and DTS.

Linux is pinned to upstream **6.18.51 LTS** without source patches.
Hyper-specific drivers build as external `.ko` modules.

## Build

On Linux, install a C toolchain with static linking support, GNU make,
Python 3.12+, bison, flex, bc, Perl, OpenSSL/libelf development headers, and kmod.

```sh
make fetch
make base JOBS=4
make test
```

Non-AArch64 builders set `CROSS_COMPILE=aarch64-linux-gnu-`.
`PLATFORM=rpi5` selects the board driver configuration; physical board and DMA
qualification remain separate. Downloaded sources and outputs stay in `target/`.
See [architecture and validation](ARCHITECTURE.md) for the image contract and
reserved-page vhost-scsi acceptance test.

The manual **Upstream kernel and real-disk acceptance** GitHub workflow rebuilds
the locked sources and external module, then verifies vhost-scsi against a
QEMU virtio-scsi disk and independently checks the backing image. Its retained
base artifact is an integration input, not a published appliance; this test
does not yet exercise the cross-VM Hyper path.

The build also creates a complete appliance under `target/io-vm/boot/`, selected
by `qemu-boot.json`, and matching source archives under `source-artifacts/`.
Its explicit `hyper.role=io` boot argument loads the bridge modules, exports
`/dev/sda` through LIO iblock, and starts the control service. `hyper.role=business`
runs an ordinary Linux disk-driver read/write/flush test against `/dev/sda`;
use that role only with a disposable acceptance disk. Missing roles power off.
The base remains available separately for isolated Linux fixtures.

The control service handles setup/reset only. Queue kicks and completions use
kernel eventfd bindings, while vhost-scsi handles requests directly. Reset
acknowledges only after endpoint drain and callback detach; a failed drain
retains its resources. Service exit powers off the I/O VM so its Native owner
can quiesce DMA before releasing imported memory. Cross-VM qualification is
still required before publishing a production package.

## Package delivery

Release complete boot images as OCI artifacts in GHCR using ORAS 1.3.
Authenticate with `oras login ghcr.io`; CI uses a token with `packages:write`.
The publisher validates the assembled kernel, initramfs, base identity, and
kernel configuration before uploading. It prints the immutable reference to
pin in HypeR; no moving `latest` tag is used by the consumer.

The GitHub workflow uses the repository's `GITHUB_TOKEN`; no registry secret is
required. Build an exact source revision first, run HypeR's cross-VM acceptance
against its retained `aarch64-qemu-base` artifact, then publish that same run:

```sh
gh workflow run kernel.yml --ref main -f operation=build
# After the build and HypeR cross-VM qualification succeed:
gh workflow run kernel.yml --ref main -f operation=publish \
  -f qualified_run=BUILD_RUN_ID -f revision=FULL_COMMIT_ID -f version=VERSION
```

Publication revalidates the build identity and sources, serializes requests for
one version, refuses an existing tag, and retains `published-reference.txt` as
an Actions artifact. HypeR pins that digest rather than the release tag. Repository
visibility and package visibility are separate: the package must allow anonymous
pull before it is adopted by public HypeR CI.

The equivalent local publisher is:

```sh
python3 -B tools/publish.py --boot /path/to/verified-boot \
  --base /path/to/verified-base --sources /path/to/sources.tar.xz \
  --revision FULL_COMMIT_ID --reference ghcr.io/roolrz/hyper-io-vm:VERSION-aarch64-qemu
```

`--oci-layout /path/to/layout` writes a local OCI package for verification
instead of uploading. Publication must follow successful appliance integration
checks; this tool alone cannot qualify a working backend.

The corresponding-source tar.xz must contain the exact upstream Linux and
BusyBox sources, module and service sources, resolved kernel/BusyBox configs,
build/assembly scripts, source locks, and license texts for the shipped build.
A URL list is insufficient. The source layer is part of the same immutable
OCI manifest as the binaries; HypeR fetches only `Image` and `initramfs.cpio.gz`.
The publisher verifies the source revision, recorded build inputs, upstream
archives, resolved configuration, and preservation of the compiled base payload.
Release review still establishes that the qualified appliance meets its runtime
contract.

## Licensing

Original build tools, configurations, and tests are Apache-2.0; see [LICENSE](LICENSE).
Linux bridge modules in `modules/` are GPL-2.0-only; see
[GPL-2.0](LICENSES/GPL-2.0-only.txt) and per-file SPDX headers. Linux and BusyBox
retain their upstream licenses. Binary releases must carry matching source
materials and notices; the combined appliance is not Apache-only.

## Standby boot

HypeR can boot `hyper.role=io hyper.mode=standby` with only assigned storage
and a control mailbox. The service probes vhost-scsi and blocks on mailbox
requests without allocating client RAM, notification bindings, or virtqueues.
HELLO and RESET remain available; ACTIVATE is rejected while no client is
provisioned. Live client attachment is not implemented by this mode. The
existing attached deployment and disk acceptance remain unchanged.

### Board volume deployments

The HypeR packager derives `/etc/hyper-volumes.conf` from the board JSON and
includes it in the bootstrap initramfs. Its first line is `hyper.volumes.v1`;
each subsequent line is `name PARTUUID sectors owner mapper`, with 512-byte
sectors. Board boots set `hyper.volumes=required` so a missing manifest is fatal.
The first volume is `config`, owned by `hyper`, mapped as `hyper-config`.

`hyper-volumes` validates the complete manifest, outer GPT checksums, partition
identity, capacity and logical sector size before creating any dm-linear volume.
Each volume receives its own LIO target; volume N uses `naa.5001405%09x` with N
starting at 1. The appliance does not mount these filesystems, scan guest GPTs,
or run udev, LVM, RAID discovery or automatic filesystem activation. Linux LIO
claims the mapped block devices while exporting them. `/etc/hyper-clients.conf` begins with `hyper.clients.v1`, followed by
`client-id volume-name` lines. Client 0 exclusively owns `config`. Duplicate
client IDs and duplicate volume assignments are rejected before creating any
exports. Each authorized client receives a separate service process, vhost fd,
LIO target, memory mapping, mailbox and notification device.

Shutdown drains vhost before removing LIO exports and dm mappings. A failed
vhost drain retains mappings until the host quarantines and destroys the I/O VM.
Old single-disk qualification fixtures without a manifest retain their legacy
whole-disk setup; they are not board deployment profiles.

For managed slots, each mailbox, notification and guest-memory DTB node carries
`hyper,client-id = <N>` (0 through 127). The devices are named
`hyper-io-control-N`, `hyper-io-notification-N` and `hyper-memory-N`. Omitting this
property preserves legacy naming and fixture behavior. The guest-memory node
reserves an alias address window and Linux page metadata; the HypeR owner must
not allocate all possible VM RAM up front or expose the window as ordinary
allocatable Linux RAM. Linux must not touch unbacked alias contents during boot.
This platform invariant still needs real Linux boot/QEMU validation.

Managed sessions add two version-1 control operations. `PREPARE_MEMORY` (5) is a
56-byte record: the existing 40-byte header followed by little-endian frontend
GPA base and byte length. The HypeR owner installs the actual pages first; the
service maps only this length, bounded by its DTB window. `RELEASE_MEMORY` (6)
is a 40-byte record. Its success reply follows synchronous vhost drain, unmap
and memory-fd closure. Only then may HypeR revoke the alias mapping and free the
grant. Failure requires quarantine. `RESET` retains the prepared memory for a
virtio device reset. Reusing a released slot with a new binding requires HELLO
and a strictly newer epoch; delayed requests from old bindings are rejected.
Older appliances reject the new operations; callers must fail rather than use
an unsafe whole-window fallback. Individual normally closed clients retire
without stopping other services; the owner must keep a slot mailbox alive when
it intends to reuse that slot via RELEASE/PREPARE.
