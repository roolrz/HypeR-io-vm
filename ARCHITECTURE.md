<!-- SPDX-FileCopyrightText: 2026 roolrz -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Linux I/O VM

This repository owns the complete Linux I/O appliance: upstream source locks,
kernel configuration, external modules, Linux userspace services, initramfs,
backend tests, and package publication. HypeR owns Native apps, DTS/DTB,
virtual hardware, and launch policy. HypeR imports a digest-pinned package;
it does not rebuild Linux. Its board packager adds versioned bootstrap volume
and client manifests to the imported rootfs; executable Linux components remain
owned and built here.

Linux remains unmodified upstream, initially 6.18.51 from the 6.18 LTS series.
`sources.lock.json` pins archive checksums. Hyper-specific drivers are external
modules built against that exact kernel. No private Linux patch set is used.

Own build recipes and tests retain Apache-2.0; `modules/` is GPL-2.0-only.
Self-authored headers shared by the userspace service and modules offer
Apache-2.0 OR GPL-2.0-only, as identified by their SPDX notices.
Downloaded Linux/BusyBox and their binaries retain upstream licenses.
Package distribution must include matching source materials, configurations,
build scripts, and notices. Repository separation does not waive these duties.

GHCR is the release boundary. Publish an OCI artifact containing the kernel,
complete initramfs, versioned manifest, and corresponding source materials.
HypeR selects an immutable digest, verifies format/platform/checksums, and
supplies its own authoritative DTB. Publication is a separate, explicitly
qualified workflow; pushing a development branch never publishes an appliance.

## Validation boundaries

The kernel workflow builds the pinned upstream kernel and both external modules,
checks control transactions, boots the business appliance and runs real-disk
vhost-scsi/LIO write/flush/read verification. HypeR's integration tests separately
exercise the cross-VM Native ownership and notification paths. A Linux-only
acceptance result does not prove those paths or Pi 5 physical DMA.

Board volume deployment adds dm-linear ownership, managed clients, Native config
storage and dynamic scatter grants. Their complete HypeR boot, mount, guest I/O
and teardown qualification must pass before publishing this development version.
Pi 5 still requires hardware qualification of its DMA, firmware and reset paths.

## Target storage protocol and control ownership

The guest-facing device uses modern virtio-mmio and standard virtio-scsi. Linux
vhost-scsi/LIO consumes the shared virtqueues. Neither vm-runtime nor the Linux
management service forwards individual storage requests.

Configuration MMIO uses a bounded per-vCPU request, published after hardware
detach and completed through the owning vCPU capability. Reading a request is
not acknowledgement. Completion matches the exact generation and advances the
instruction exactly once. Administrative stop cancels the continuation.
Only registered device ranges may use this path; unknown MMIO retains its
diagnostic terminal disposition.

Queue kicks and completion interrupt status/acknowledgement use prevalidated
kernel notification bindings. They do not enter the userspace device loop.
Durable pending state and virtqueue indices are authoritative; a physical IPI
is only a prompt to a remote CPU. Idle backends block, and notification arming
must recheck pending work to avoid lost wakeups.

Virtio features are the intersection of the guest transport, bridge, and Linux
backend capabilities. Queue activation commits only after mappings and backend
configuration have succeeded. Device reset first disables new submissions,
quiesces the backend, and then releases resources. Linux vhost ioctl numbers
remain Linux-local: the cross-VM bridge is not an ioctl forwarding ABI.

## Shared pages and DMA

Direct queue consumption requires Linux to access queue metadata, responses,
and every guest data buffer referenced by descriptors. The trusted deployment
may grant an entire business VM's RAM without requiring physical contiguity. Native HypeR storage clients instead use
an explicitly shared I/O pool; the rest of HypeR memory is not exported.

The same physical pages must remain owned and stable until all backend CPU and
DMA users have retired. Guest physical, I/O VM physical, Linux virtual, host
physical, and device DMA addresses are distinct domains. A stage-2 alias does
not translate a physical device's DMA transaction.

Dynamic grants expose an immutable kernel-owned extent manifest only after
route-bound, one-use token admission. Linux creates a contiguous userspace view
from the authorized pages and shares reference-counted 2 MiB device-page metadata
across grants. Ordinary guest pages retain 4 KiB allocation granularity. Actual
grant aliases use a fixed affine host-address mapping represented by dma-ranges;
unused aperture addresses are neither ordinary Linux RAM nor preallocated guest
storage. Static config storage uses its bounded reserved pool.

Release drains vhost, unmaps the client view, retires its metadata references,
and writes a guest-origin quiescence proof. Only then may Native ownership
release the mapping and flush stage-2 translations. The register and bootstrap
formats are documented in [README](README.md#board-volume-deployments).

A successful mmap or shared-memory copy is insufficient validation: actual
vhost scatterlists and device DMA must be exercised. Device alignment constraints
may still require bounce buffers; no end-to-end zero-copy claim follows solely
from the shared-memory transport.

Stopping I/O VM vCPUs does not stop physical DMA. On a platform without DMA
isolation, pages cannot be reclaimed after a crash until the assigned hardware
has been reset and outstanding transactions have been drained. If quiescence
cannot be established, quarantine the pages; a timeout never grants permission
to reuse them.

## Building and validating the base

On a Linux build host with a C toolchain, GNU make, Python 3.12+, bison, flex,
bc, Perl, OpenSSL development headers, libelf development headers, and kmod:

```sh
make base
# A non-AArch64 Linux host also supplies CROSS_COMPILE=aarch64-linux-gnu-
```

The compiler must support static userspace linking for the minimal BusyBox
rootfs. `PLATFORM=rpi5` adds the platform's upstream driver configuration;
it does not assign physical devices or claim that board qualification passed.
`make fetch` works on non-Linux build hosts too. Downloaded archives are
checked against `sources.lock.json` before extraction. No patches are
applied to either source tree.

`target/io-vm/qemu-base.json` identifies an immutable artifact directory and its
manifest digest. That directory contains `Image`, `base.cpio.gz`, the resolved
kernel configuration, and `manifest.json`. Build outputs are separate from the
downloaded source. Re-running the build uses upstream Kbuild dependencies.

The default build assembles the kernel, static BusyBox, both external modules
and service using this repository’s `rootfs/init`; `qemu-boot.json` identifies
the complete artifact. To explicitly assemble a separately reviewed overlay:

```sh
python3 -B scripts/assemble-io-vm.py \
  --base /path/to/verified-base --manifest-sha256 MANIFEST_SHA256 \
  --overlay /path/to/product-overlay --output target/io-vm/boot
```

The default build budgets are 32 MiB for the raw Linux Image, 8 MiB for the
compressed ramdisk, and 32 MiB for its expanded newc archive. Assembly publishes
content-addressed kernel/ramdisk files and commits `boot-artifacts.json` last.
Consumers use that manifest rather than assuming that independently replaced
`Image` and `initramfs` names identify a coherent generation.

The Linux-only `tests/io-vm/vhost-scsi.c` fixture builds with a static C compiler
and Linux UAPI headers. `tests/io-vm/verify.py` accepts a base directory, pinned
manifest digest, and test binary. It boots the new kernel with a DT-reserved
RAM region and a newly created disposable QEMU SCSI disk, loads the external
module, and performs 32 write/flush/read cycles through upstream vhost-scsi/LIO.
The host independently checks disk contents. This qualifies Linux reserved-page
import; it does not yet qualify Hyper cross-VM notification or physical DMA.

## Compatibility and qualification

Before release, bridge layouts must define explicit widths, endianness, lengths, capability
bits, and generation-tagged identities. Extensions are negotiated; existing
fields and operation numbers do not change meaning. Unsupported mandatory
features fail connection setup. The Native syscall ABI and Linux-local UAPI
are separate interfaces and do not inherit bridge version numbers.

Qualification covers single/multiple vCPUs, notification-before-wait races,
concurrent queue reset, runtime exit, I/O VM exit with in-flight operations,
stale completions, and actual read/write/flush error propagation. QEMU validates
the integration; Pi 5 must additionally validate DMA address translation,
cache maintenance, interrupt ordering, device reset, and measured image sizes.
