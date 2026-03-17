# Makefile - Convenience targets for hwmond
#
# Primary build method: zig cc (produces native dynamically-linked ESXi binary)
# Alternative: native build (for testing on macOS/Linux)

.PHONY: all zig native clean vib deploy test help

BUILD_DIR    = build
BINARY       = $(BUILD_DIR)/hwmond
VIB_FILE     = $(BUILD_DIR)/hwmond-xserve.vib

# Default target: build via zig for ESXi
all: zig

# ---- Zig cross-compile (dynamic glibc binary for ESXi) ----

zig:
	@echo "==> Building dynamic glibc binary via zig cc..."
	@mkdir -p $(BUILD_DIR)
	zig cc -target x86_64-linux-gnu.2.12 -O2 -o $(BINARY) src/main.c src/panel_usb.c src/cpu_usage.c src/bmc.c -lpthread -lm -lrt
	@echo "==> Binary: $(BINARY)"
	@file $(BINARY)
	@ls -lh $(BINARY)

# ---- Native build (for local testing, not for ESXi) ----

native:
	@echo "==> Building native binary..."
	@mkdir -p $(BUILD_DIR)
	cd $(BUILD_DIR) && cmake .. -DCMAKE_BUILD_TYPE=Debug && make -j$$(nproc 2>/dev/null || sysctl -n hw.ncpu)
	@echo "==> Binary: $(BINARY)"

# ---- VIB packaging ----

vib: zig
	@echo "==> Packaging VIB..."
	./scripts/build-vib.sh $(BINARY) $(VIB_FILE)
	@echo "==> VIB: $(VIB_FILE)"
	@ls -lh $(VIB_FILE)

# ---- Deploy to ESXi (requires ESXI_HOST env var) ----

deploy: vib
ifndef ESXI_HOST
	$(error Set ESXI_HOST=<ip-or-hostname> to deploy)
endif
	@echo "==> Deploying VIB to $(ESXI_HOST)..."
	scp $(VIB_FILE) root@$(ESXI_HOST):/tmp/hwmond-xserve.vib
	ssh root@$(ESXI_HOST) 'esxcli software acceptance set --level CommunitySupported && esxcli software vib install -v /tmp/hwmond-xserve.vib --force --no-sig-check --no-live-install && echo "VIB installed. Reboot required."'

# ---- Test mode (native build, for dev machines with libusb) ----

test: native
	@echo "==> Running in test mode (requires Xserve USB panel)..."
	$(BINARY) -t

# ---- Clean ----

clean:
	rm -rf $(BUILD_DIR)

# ---- Help ----

help:
	@echo "hwmond - Xserve Front Panel LED Daemon for ESXi"
	@echo ""
	@echo "Targets:"
	@echo "  make          Build native ESXi binary via zig cc (default)"
	@echo "  make zig      Build native ESXi binary via zig cc"
	@echo "  make native   Build native binary (for local testing)"
	@echo "  make vib      Build binary + package as ESXi VIB (v2.0.0)"
	@echo "  make deploy   Build VIB + install on ESXi host (set ESXI_HOST=...)"
	@echo "  make test     Build native + run in test mode"
	@echo "  make clean    Remove build artifacts"
	@echo ""
	@echo "Environment variables:"
	@echo "  ESXI_HOST     ESXi hostname/IP for deploy target"
