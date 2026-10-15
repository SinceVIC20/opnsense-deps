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
├── config/               # FMC/CDX XML configuration files, build-fixes/ patches
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

These steps build OPNsense 26.7.7 (FreeBSD 15.1-RELEASE-p4) for the Mono Gateway from the
`mono-gateway-dev` branches of
[SinceVIC20/opnsense-src](https://github.com/SinceVIC20/opnsense-src) and
[SinceVIC20/opnsense-deps](https://github.com/SinceVIC20/opnsense-deps).
The kernel, base and board package are built from source, step by step. The
packages set can be downloaded from the temporary update server at
mono-opnsense.sincevic20.com instead of built, which saves several hours.
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
git clone https://github.com/opnsense/ports /usr/ports
```

Set tools, core, plugins and ports to the OPNsense 26.7.7 release. `reset`
keeps each repo on its branch, which the tools check for:

```
git -C /usr/tools reset -q --hard 26.7.7
```

```
git -C /usr/core reset -q --hard 26.7.7
```

```
git -C /usr/plugins reset -q --hard 26.7.7
```

```
git -C /usr/ports reset -q --hard 26.7.7
```

Then add the board file:

```
cp /usr/deps/config/GATEWAY.conf /usr/tools/device/
```

### 3. Fixes needed on top of tools and ports

The package build needs a few changes on aarch64. Apply them with:

```
git -C /usr/tools apply /usr/deps/config/build-fixes/tools-26.7.7.patch
```

```
git -C /usr/ports apply /usr/deps/config/build-fixes/ports-26.7.7.patch
```

What they change:

- `/usr/tools/config/26.7/make.conf`: adds `MILTER` to `OPTIONS_UNSET`.
- `/usr/tools/config/26.7/ports.conf` and `plugins.conf`: comments out the
  zabbix 6 and 7.4 ports and puppet; adds `textproc/rubygem-rexml` to
  `ports.conf`.
- `security/nss`: fixes a duplicate-symbol link error on aarch64
  (`platform_ghash_support` and the `gcm_Hash*_hw` functions defined twice).
- `sysutils/sysinfo`: new checksum. Codeberg repackaged the 1.0.4 download
  with the same contents.

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

```
make -C /usr/deps safety-package SAFETY_RELEASE=26.7.7_3
```

The cmm build has no header dependencies, so always remove its objects first.
Always build through this top-level Makefile, not a component's own Makefile.
Use the newest release on the update server as `SAFETY_RELEASE`.
The results are `/usr/deps/dist/mono-gateway-<version>.pkg` and
`mono-upgrade-safety-<version>.pkg`. The image needs both: mono-gateway
depends on the safety package, and without it the image is built without
mono-gateway (no offload).

### 6. Packages set

Download the packages set from the temporary update server instead of building it:

```
make -C /usr/tools prefetch-packages MIRRORS=https://mono-opnsense.sincevic20.com VERSION=26.7.7_3 SETTINGS=26.7 TOOLSDIR=/usr/tools
```

Use the newest `packages-*` version listed at
https://mono-opnsense.sincevic20.com/FreeBSD:15:aarch64/26.7/sets/. Skip this
step to build the packages set yourself from ports, core and plugins (several
hours, done by the first `arm-5G` below).

### 7. Image

```
make -C /usr/tools arm-5G DEVICE=GATEWAY SETTINGS=26.7 TOOLSDIR=/usr/tools SRCBRANCH=mono-gateway-dev
```

```
make -C /usr/deps compress-image OPS_SETTINGS=26.7
```

If there is no packages set yet, the first `arm-5G` builds it, and later
builds reuse it.

`compress-image` zeroes the image's free space and writes
`OPNsense-*-arm-aarch64-GATEWAY.img.bz2` with `.sha256` and `.md5` files.
Images use a UFS root (label `OPNsense_ARM`). Flash the `.img.bz2` with
[mono-imager](https://github.com/HAHermsen/mono-imager). It must be bzip2:
mono-imager only handles `.img.bz2`, and with any other format (such as
`.img.gz`) it fails after it has already erased the eMMC.

### 8. Update server releases

To publish kernel and system updates to a separate update server, build the
safe-upgrade package with `make -C /usr/deps safety-package SAFETY_RELEASE=<release>`
and use the scripts in `scripts/` (`bump-opnsense-update.sh`,
`publish-update-server.sh`, `build-upgrade-set.sh`, `publish-changelog.sh`).
They sign with that server's own key, and routers then need its fingerprint
(see `scripts/update-server/use-temp-updates.sh`).

## Flashing and first boot

Follow the [install guide](https://mono-opnsense.sincevic20.com/images/install-guide.html).
It flashes the `.img.bz2` with mono-imager, which keeps the eMMC firmware in
place, and sets U-Boot to boot OPNsense. Don't `dd` an image onto the start of
`/dev/mmcblk0`: that overwrites the firmware stored in its first 32 MB.

## Development workflow

### Kernel source

Kernel changes are commits on the `mono-gateway-dev` branch of
[SinceVIC20/opnsense-src](https://github.com/SinceVIC20/opnsense-src), on top of
Mono's 26.7 port in the `stable/26.7` branch. To see them:

```
git -C /usr/src log --oneline origin/stable/26.7..HEAD
```

After a change, rebuild the kernel set (step 4, move the old set out first) and
the board package (step 5), since the modules build against this source.

### Out-of-tree modules

Drivers in `drivers/` and `fastpath/` build against the kernel source in
`/usr/src`. To rebuild one, or all of them:

```
make -C /usr/deps build-cdx
```

```
make -C /usr/deps -j5 modules
```

To test them on a router, copy the `.ko` over by hand. To ship them, build the
board package as in step 5 (`make all` is what copies them into `dist/`).

> ## Disclaimer
> Many of the files in this repository, and in the [kernel fork](https://github.com/we-are-mono/opnsense-src), were ported from Linux sources provided by NXP (originally targeting Linux kernel 5.4). Due to the sheer scope of the porting effort, Claude Opus 4.6 was used to assist with the adaptation of this code to FreeBSD. As such, this project should be considered **highly experimental** and is **not fit for production use** until thorough reviews and testing have been performed by the wider community.
