<!-- SPDX-FileCopyrightText: 2026 roolrz -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Guest-memory retirement regressions

`hyper_granules_test.ko` compiles the production granule registry and retirement
implementation into a separate test module. It uses real Linux resource claims,
mutexes, xarrays, workqueues, and joins. Only `memremap_pages` and
`memunmap_pages` are replaced with controlled import/release callbacks; no test
maps or accesses the claimed physical addresses.

The cases cover empty and partial batches, 133 independent granules, surviving
shared owners, concurrent callers with overlapping ownership, exclusion of
readmission during retirement, and cleanup after an import failure beyond the
first batch. Completion barriers hold a release while another proceeds and
verify that neither its caller nor the next batch can finish early. The
timeouts detect a stuck test; there are no performance thresholds.

Build against the same configured upstream source and output used for the
appliance, then supply the resulting module to the existing acceptance runner:

```sh
make -C "$LINUX_SOURCE" O="$LINUX_OUTPUT" ARCH=arm64 LLVM=1 \
    M="$PWD/tests/guest-memory" MO="$TEST_OUTPUT" modules
python3 -B tests/io-vm/verify.py \
    --base "$BASE" --manifest-sha256 "$MANIFEST_SHA256" \
    --test-binary "$VHOST_SCSI_TEST" \
    --retirement-module "$TEST_OUTPUT/hyper_granules_test.ko"
```

The runner requires the module's success marker and unloads it before the
existing vhost-scsi fixture. Actual foreign-page reference draining and DMA
quiescence still require the HypeR broker lifecycle tests with the real driver.
Teardown timing is measured separately from these functional checks.
