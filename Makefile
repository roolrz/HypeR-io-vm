# SPDX-FileCopyrightText: 2026 roolrz
# SPDX-License-Identifier: Apache-2.0

OUTPUT ?= $(CURDIR)/target/io-vm
PLATFORM ?= qemu
JOBS ?= 4
CROSS_COMPILE ?=

.PHONY: fetch base all-platforms test
fetch:
	python3 -B tools/build.py --output "$(OUTPUT)" --fetch-only

base:
	python3 -B tools/build.py --output "$(OUTPUT)" --platform "$(PLATFORM)" \
		--jobs "$(JOBS)" --cross-compile "$(CROSS_COMPILE)"

# Separate outputs keep each profile and its corresponding sources coherent.
all-platforms:
	$(MAKE) base PLATFORM=qemu OUTPUT="$(OUTPUT)/qemu"
	$(MAKE) base PLATFORM=rpi5 OUTPUT="$(OUTPUT)/rpi5"

test:
	python3 -B tests/build/io-vm.py
	python3 -B tests/build/publish.py
	python3 -B tests/build/volumes.py
