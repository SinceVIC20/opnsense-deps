# Mono Gateway custom components for OPNsense

Out-of-tree kernel modules, userspace daemons, and build tooling for the Mono
Gateway OPNsense image. Contains the DPAA1 fast-path stack (CDX flow offload,
FCI, CMM connection manager, PF notifier, auto-bridge), board-support drivers
(LP5812 LED, EMC2302 fan, INA2xx power, SFP LED, PCF2131 RTC, TMP431 thermal,
CAAM crypto), WiFi support (NXP 88W9098 PCIe driver + DPAA OH port bridge),
FMan toolchain (fmlib, fmc, dpa_app), and helper scripts for kernel image
creation and deployment.

## Repository layout

```
opnsense-deps/
├── drivers/              # out-of-tree kernel modules
│   ├── caam/             # NXP CAAM crypto accelerator (JR + QI)
│   ├── dpaa_wifi/        # WiFi ↔ FMan OH port data plane bridge
│   ├── emc2302/          # SMSC EMC2302 fan controller
│   ├── fand/             # Fan control daemon (userspace)
│   ├── ina2xx/           # TI INA2xx power monitor
│   ├── lp5812/           # TI LP5812 LED controller
│   ├── mwifiex/          # NXP 88W9098 PCIe WiFi driver + firmware
│   ├── pcf2131/          # NXP PCF2131 RTC
│   ├── sfp-led/          # SFP+ link status LED
│   └── tmp431/           # TI TMP431 thermal sensor
├── fastpath/             # DPAA1 fast-path modules + userspace
│   ├── cdx/              # CDX flow offload kernel module
│   ├── fci/              # Fast-path Control Interface module
│   ├── auto_bridge/      # Auto-bridge kernel module
│   ├── pf_notify/        # PF state change notifier module
│   ├── fmlib/            # FMan userspace library
│   ├── fmc/              # FMan Configuration tool
│   ├── dpa_app/          # DPAA application (PCD setup)
│   ├── cmm/              # Connection Manager daemon
│   └── cmmctl/           # CMM control utility
├── config/               # FMC/CDX XML configuration files
├── rc.d/                 # rc.d service scripts and syshook scripts
├── pkg/                  # FreeBSD package metadata
├── _vendor/              # NXP vendor repos (cloned at build time, gitignored)
└── Makefile              # top-level build (modules + userspace + package)
```

## Network interface mapping

The FMan MAC numbering in the device tree does not match the physical port order
on the board. This is a cosmetic mismatch in the PCB routing — it has no effect
on performance or functionality.

| Physical position (left → right) | Interface | Speed |
|----------------------------------|-----------|-------|
| Port 1 | dtsec1 | 1 GbE |
| Port 2 | dtsec2 | 1 GbE |
| Port 3 | dtsec0 | 1 GbE |
| Port 4 | dtsec3 | 10 GbE SFP+ |
| Port 5 | dtsec4 | 10 GbE SFP+ |

## Build instructions

These steps build OPNsense 26.7 (FreeBSD 15.1) for the Mono Gateway from the
`mono-gateway-dev` branches of
[SinceVIC20/opnsense-src](https://github.com/SinceVIC20/opnsense-src) and
[SinceVIC20/opnsense-deps](https://github.com/SinceVIC20/opnsense-deps).
Everything is built from source, step by step. The prefetch steps from Mono's
original instructions don't apply (Mono's package mirror is gone), and
`make -C /usr/deps image` is broken: don't use it, it empties `dist/`.

### 1. Build server

FreeBSD 15.1 on aarch64 (a VM on Apple silicon works well), about 6 cores and
a 120 GB disk. Install these once, before the first build:

```
pkg install -y git python3 aarch64-binutils libxml2
```

After that, don't install or upgrade packages on the build server. pkg
updates itself to a newer version than the ports tree expects, and the
OPNsense tools then stop with "Installed pkg version ... does not match
required version". To fix that:

```
make -C /usr/ports/ports-mgmt/pkg -DBATCH clean all reinstall
```

### 2. Repositories

```
git clone -b mono-gateway-dev https://github.com/SinceVIC20/opnsense-src /usr/src
```

```
git clone -b mono-gateway-dev https://github.com/SinceVIC20/opnsense-deps /usr/deps
```

```
git clone https://github.com/opnsense/tools /usr/tools
```

```
git clone -b stable/26.7 https://github.com/opnsense/core /usr/core
```

```
git clone -b stable/26.7 https://github.com/opnsense/plugins /usr/plugins
```

```
git clone -b 2026Q3 https://git.FreeBSD.org/ports.git /usr/ports
```

The ports tree also needs the `opnsense/` category from
[opnsense/ports](https://github.com/opnsense/ports): clone it elsewhere and
copy its `opnsense/*` into `/usr/ports/opnsense/`. Then add the board file:

```
cp /usr/deps/config/GATEWAY.conf /usr/tools/device/
```

### 3. Fixes needed on top of tools and ports

The package build needs a few local changes, because FreeBSD's quarterly
ports have moved on from what OPNsense 26.7's settings expect:

- `/usr/tools/config/26.7/make.conf`: add `MILTER` to `OPTIONS_UNSET`, and
  build `math/py-numpy` with `OPENBLAS` (its `NOBLAS` option no longer exists).
- `/usr/tools/config/26.7/ports.conf` and `plugins.conf`: comment out the
  zabbix 6 and 7.4 ports and puppet, which no longer build; add
  `textproc/rubygem-rexml` to `ports.conf`.
- `security/nss`: `files/patch-coreconf_FreeBSD.mk` needs a fix for a
  duplicate-symbol link error on aarch64 (`platform_ghash_support` and the
  `gcm_Hash*_hw` functions defined twice).
- `security/openssh-portable`: replace the port with the one from
  opnsense/ports tag `26.7.4` (OpenSSH 10.5p1). OPNsense core 26.7.3 and later
  always write an ML-DSA key setting into `sshd_config`, and the 2026Q3 port
  (10.3p1) refuses to start with it.

### 4. Kernel and base sets

The tools reuse any kernel or base set already in
`/usr/local/opnsense/build/26.7/aarch64/sets/` without checking it matches the
source. Move the old set out of that folder before rebuilding it.

```
make -C /usr/tools kernel DEVICE=GATEWAY SETTINGS=26.7 TOOLSDIR=/usr/tools SRCBRANCH=mono-gateway-dev
```

```
make -C /usr/tools base DEVICE=GATEWAY SETTINGS=26.7 TOOLSDIR=/usr/tools SRCBRANCH=mono-gateway-dev
```

The set names end in the source commit they were built from.

### 5. Board package (this repo)

```
rm -rf /usr/deps/_build/cmm
```

```
make -C /usr/deps clean
```

```
make -C /usr/deps -j5 all
```

```
make -C /usr/deps package
```

The cmm build has no header dependencies, so always remove its objects first.
Always build through this top-level Makefile, not a component's own Makefile.
The result is `/usr/deps/dist/mono-gateway-<version>.pkg`.

### 6. Image

```
make -C /usr/tools arm-5G DEVICE=GATEWAY SETTINGS=26.7 TOOLSDIR=/usr/tools SRCBRANCH=mono-gateway-dev PORTSBRANCH=2026Q3
```

```
make -C /usr/deps compress-image OPS_SETTINGS=26.7
```

The first `arm-5G` also builds the packages set from ports, core and plugins,
which takes several hours; later builds reuse it. Afterwards, check the
packages set has `openssh-portable` 10.4 or newer.

`compress-image` zeroes the image's free space and writes
`OPNsense-*-arm-aarch64-GATEWAY.img.bz2` with `.sha256` and `.md5` files.
Images use a UFS root (label `OPNsense_ARM`). Flash the `.img.bz2` with
[mono-imager](https://github.com/HAHermsen/mono-imager). It must be bzip2:
mono-imager only handles `.img.bz2`, and with any other format (such as
`.img.gz`) it fails after it has already erased the eMMC.

### 7. Update server releases

To publish kernel and system updates to your own update server, build the
safe-upgrade package with `make -C /usr/deps safety-package SAFETY_RELEASE=<release>`
and use the scripts in `scripts/` (`bump-opnsense-update.sh`,
`publish-update-server.sh`, `build-upgrade-set.sh`, `publish-changelog.sh`).
They sign with your own key, and routers then need that key's fingerprint
(see `scripts/update-server/use-temp-updates.sh`).

## Development workflow

### Kernel source

The kernel source (`opnsense-src`) is a fork of OPNsense/FreeBSD at
[we-are-mono/opnsense-src](https://github.com/we-are-mono/opnsense-src),
branched from `stable/26.1`. All Mono Gateway changes live as commits on this
branch.

**Making a change:**

```bash
cd /usr/src

# See Mono Gateway commits on top of upstream
git log --oneline origin/stable/26.1..HEAD

# 1. Edit the file
vim sys/contrib/device-tree/src/arm64/freescale/mono-gateway-dk.dts

# 2. Stage and commit
git add sys/contrib/device-tree/src/arm64/freescale/mono-gateway-dk.dts
git commit -m "dts: add PCIe WiFi reset GPIO"

# 3. Push
git push
```

To rebase on a newer upstream OPNsense release:

```bash
git remote add upstream https://github.com/opnsense/src.git
git fetch upstream
git rebase upstream/stable/26.7   # or whatever the new branch is
git push --force-with-lease
```

### Out-of-tree modules

Drivers in `drivers/` build against kernel headers without modifying the kernel
source. To iterate on one:

```bash
# Rebuild just caam
sudo make -C /usr/deps build-caam

# Or rebuild all modules
sudo make -C /usr/deps -j24 modules
```

Output `.ko` files land in the driver directory and are collected into `dist/`
by `make all`.

## Local / Test Deployment

Flashing writes the image directly to the gateway's eMMC. The gateway must be
booted into Recovery Linux (initramfs) so the eMMC is not in use.

**Warning:** This overwrites the entire eMMC, including any existing OPNsense
installation and its configuration.

### 1. Boot into Recovery Linux

Power on the gateway and let U-Boot autoboot into Recovery Linux (or interrupt
U-Boot and run `run recovery`).

### 2. Configure networking on the gateway

Recovery Linux has no DHCP client. Assign a static IP on the uplink port
(adjust addresses to match your network):

```
ip link set eth3 up
ip addr add 10.0.0.69/24 dev eth3
ip route add default via 10.0.0.1 dev eth3
```

### 3. Serve the image from the build server

On the build server, start an HTTP server in the images directory:

```
cd /usr/local/opnsense/build/26.1/aarch64/images
python3 -m http.server 8000
```

### 4. Flash the eMMC

On the gateway, stream the compressed image directly to the eMMC. This
downloads, decompresses, and writes in a single pipeline — no local storage
needed (works even on devices with limited RAM):

```
curl http://[build-server]:8000/OPNsense-*-GATEWAY.img.bz2 | bzip2 -dc | dd of=/dev/mmcblk0 bs=1M
```

For an uncompressed image (slower transfer, no bzip2 needed):

```
curl http://[build-server]:8000/OPNsense-*-GATEWAY.img | dd of=/dev/mmcblk0 bs=1M
```

### 5. Reboot

```
reboot
```

U-Boot will load `kernel.img` and the device tree from the FAT partition and
boot into OPNsense.

## Booting into OPNsense

The board's default boot command loads OpenWRT. To boot OPNsense instead,
interrupt U-Boot's autoboot (press any key during the countdown) and configure
a persistent `opnsense` boot environment:

```
=> setenv opnsense "load mmc 0:1 ${kernel_addr_r} kernel.img; load mmc 0:1 ${fdt_addr_r} dtb/mono-gateway-dk.dtb; booti 0x82000000 - 0x88000000"
=> setenv bootcmd_bak ${bootcmd}
=> setenv bootcmd "run opnsense || run recovery"
=> saveenv
=> reset
```

This saves the original boot command as `bootcmd_bak`, then sets `bootcmd` to
try OPNsense first and fall back to Recovery Linux if the kernel image is missing.
After `saveenv` and `reset`, the board will automatically boot into OPNsense on
every subsequent power-on.

> ## Disclaimer
> Many of the files in this repository, and in the [kernel fork](https://github.com/we-are-mono/opnsense-src), were ported from Linux sources provided by NXP (originally targeting Linux kernel 5.4). Due to the sheer scope of the porting effort, Claude Opus 4.6 was used to assist with the adaptation of this code to FreeBSD. As such, this project should be considered **highly experimental** and is **not fit for production use** until thorough reviews and testing have been performed by the wider community.
