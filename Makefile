# Makefile - Build targets for hwmond
#
# ESXi:  cross-compile via zig cc → VIB package
# Linux: native compile via gcc → .deb / .rpm / install.sh

.PHONY: all esxi linux clean vib deb deploy help

BUILD_DIR    = build
BINARY       = $(BUILD_DIR)/hwmond
VIB_FILE     = $(BUILD_DIR)/hwmond-xserve.vib
DEB_FILE     = $(BUILD_DIR)/hwmond-xserve-3.0.0.deb

# ESXi source files
ESXI_SRC = src/main.c src/panel_usb.c src/cpu_usage.c src/bmc.c
# Linux source files
LINUX_SRC = src/main.c src/panel_usb.c src/cpu_linux.c src/collect_linux.c src/bmc.c

# Default target
all: esxi

# ---- ESXi (cross-compile via zig) ----

esxi:
	@echo "==> Building ESXi binary via zig cc..."
	@mkdir -p $(BUILD_DIR)
	zig cc -target x86_64-linux-gnu.2.12 -D__ESXI__ -O2 -Wall \
	  -o $(BINARY) $(ESXI_SRC) -lpthread -lm -lrt
	@echo "==> Binary: $(BINARY)"
	@file $(BINARY)
	@ls -lh $(BINARY)

# Backward compat
zig: esxi

# ---- Linux (native compile) ----

linux:
	@echo "==> Building Linux binary..."
	@mkdir -p $(BUILD_DIR)
	gcc -D__LINUX__ -O2 -Wall \
	  -o $(BINARY) $(LINUX_SRC) -lpthread -lm -lrt
	@echo "==> Binary: $(BINARY)"
	@file $(BINARY)
	@ls -lh $(BINARY)

# ---- ESXi VIB packaging ----

vib: esxi
	@echo "==> Packaging VIB..."
	./scripts/build-vib.sh $(BINARY) $(VIB_FILE)
	@echo "==> VIB: $(VIB_FILE)"
	@ls -lh $(VIB_FILE)

# ---- Linux .deb packaging ----

deb: linux
	@echo "==> Packaging .deb..."
	@mkdir -p $(BUILD_DIR)/deb-root/usr/local/sbin
	@mkdir -p $(BUILD_DIR)/deb-root/etc/systemd/system
	@mkdir -p $(BUILD_DIR)/deb-root/etc/udev/rules.d
	@mkdir -p $(BUILD_DIR)/deb-root/DEBIAN
	cp $(BINARY) $(BUILD_DIR)/deb-root/usr/local/sbin/hwmond
	chmod 755 $(BUILD_DIR)/deb-root/usr/local/sbin/hwmond
	cp scripts/hwmond.service $(BUILD_DIR)/deb-root/etc/systemd/system/
	cp scripts/99-xserve-panel.rules $(BUILD_DIR)/deb-root/etc/udev/rules.d/
	@echo "Package: hwmond-xserve" > $(BUILD_DIR)/deb-root/DEBIAN/control
	@echo "Version: 3.0.0" >> $(BUILD_DIR)/deb-root/DEBIAN/control
	@echo "Architecture: amd64" >> $(BUILD_DIR)/deb-root/DEBIAN/control
	@echo "Maintainer: hwmond" >> $(BUILD_DIR)/deb-root/DEBIAN/control
	@echo "Depends: dmidecode" >> $(BUILD_DIR)/deb-root/DEBIAN/control
	@echo "Recommends: ipmitool, smartmontools, ethtool" >> $(BUILD_DIR)/deb-root/DEBIAN/control
	@echo "Section: admin" >> $(BUILD_DIR)/deb-root/DEBIAN/control
	@echo "Priority: optional" >> $(BUILD_DIR)/deb-root/DEBIAN/control
	@echo "Description: Apple Xserve hardware monitor for Linux" >> $(BUILD_DIR)/deb-root/DEBIAN/control
	@echo " Drives front panel CPU activity LEDs and populates BMC" >> $(BUILD_DIR)/deb-root/DEBIAN/control
	@echo " for Apple Server Monitor on Xserve 3,1 servers." >> $(BUILD_DIR)/deb-root/DEBIAN/control
	@printf '#!/bin/sh\nsystemctl daemon-reload\nudevadm control --reload-rules 2>/dev/null\nudevadm trigger 2>/dev/null\nmodprobe ipmi_devintf 2>/dev/null || true\nmodprobe ipmi_si 2>/dev/null || true\nsystemctl enable hwmond\nsystemctl start hwmond\n' > $(BUILD_DIR)/deb-root/DEBIAN/postinst
	@chmod 755 $(BUILD_DIR)/deb-root/DEBIAN/postinst
	@printf '#!/bin/sh\nsystemctl stop hwmond 2>/dev/null\nsystemctl disable hwmond 2>/dev/null\n' > $(BUILD_DIR)/deb-root/DEBIAN/prerm
	@chmod 755 $(BUILD_DIR)/deb-root/DEBIAN/prerm
	dpkg-deb --build $(BUILD_DIR)/deb-root $(DEB_FILE)
	@echo "==> .deb: $(DEB_FILE)"
	@ls -lh $(DEB_FILE)

# ---- Deploy to ESXi ----

deploy: vib
ifndef ESXI_HOST
	$(error Set ESXI_HOST=<ip-or-hostname> to deploy)
endif
	@echo "==> Deploying VIB to $(ESXI_HOST)..."
	scp $(VIB_FILE) root@$(ESXI_HOST):/tmp/hwmond-xserve.vib
	ssh root@$(ESXI_HOST) 'esxcli software acceptance set --level CommunitySupported && esxcli software vib install -v /tmp/hwmond-xserve.vib --force --no-sig-check --no-live-install && echo "VIB installed. Reboot required."'

# ---- Clean ----

clean:
	rm -rf $(BUILD_DIR)

# ---- Help ----

help:
	@echo "hwmond - Apple Xserve Hardware Monitor"
	@echo ""
	@echo "ESXi targets:"
	@echo "  make esxi     Cross-compile for ESXi 6.5 via zig cc"
	@echo "  make vib      Build + package as ESXi VIB"
	@echo "  make deploy   Build VIB + install on ESXi (set ESXI_HOST=...)"
	@echo ""
	@echo "Linux targets:"
	@echo "  make linux    Build native Linux binary"
	@echo "  make deb      Build + package as Debian .deb"
	@echo ""
	@echo "Other:"
	@echo "  make clean    Remove build artifacts"
	@echo "  make help     Show this help"
