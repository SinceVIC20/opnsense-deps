#!/bin/sh
# Build the update server folder from the normal build output, signed with our key.
# The build's own sets are only read, never changed.
# Usage: publish-update-server.sh <release> <outdir>
#   <release> must match RELEASE in the opnsense-update package being served (e.g. 26.7.2).
#   For a newer release, set OPNSENSE_UPDATE_PKG to an opnsense-update package
#   made by bump-opnsense-update.sh; it replaces the one in latest/.

set -e

RELEASE=${1:?usage: publish-update-server.sh <release> <outdir>}
OUT=${2:?usage: publish-update-server.sh <release> <outdir>}

KEYDIR=${KEYDIR:-/root/signing}
SETSDIR=${SETSDIR:-/usr/local/opnsense/build/26.7/aarch64/sets}
MONOPKG=${MONOPKG:-$(ls /usr/deps/dist/mono-gateway-*.pkg)}
SAFETYPKG=${SAFETYPKG:-$(ls /usr/deps/dist/mono-upgrade-safety-*.pkg)}
SERIES=26.7
ABI=FreeBSD:15:aarch64
ARCH=aarch64
DEVICE=GATEWAY

PUB=${KEYDIR}/repo.pub
KEY=${KEYDIR}/repo.key
SIGNCMD="/usr/tools/scripts/pkg_sign.sh ${PUB} ${KEY}"
XPREFIX=aarch64-unknown-freebsd15.0

[ -f "${KEY}" ] || { echo "ERROR: no signing key in ${KEYDIR}"; exit 1; }
[ -f "${MONOPKG}" ] || { echo "ERROR: expected one mono-gateway package, got: ${MONOPKG}"; exit 1; }
[ -f "${SAFETYPKG}" ] || { echo "ERROR: expected one mono-upgrade-safety package (make safety-package), got: ${SAFETYPKG}"; exit 1; }
[ "$(tar -xOf ${SAFETYPKG} /usr/local/share/mono-upgrade/release 2>/dev/null)" = "${RELEASE}" ] || { echo "ERROR: ${SAFETYPKG} does not carry release ${RELEASE}"; exit 1; }
[ ! -e "${OUT}" ] || { echo "ERROR: ${OUT} already exists"; exit 1; }

find_one()
{
	set -- ${SETSDIR}/${1}
	[ $# -eq 1 ] && [ -f "$1" ] || { echo "ERROR: expected exactly one ${SETSDIR}/${1}" >&2; exit 1; }
	echo "$1"
}

sign_file()
{
	sha256 -q "${1}" | ${SIGNCMD} > "${1}.sig"
}

sign_repo()
{
	rm -f "${1}/meta" "${1}/meta.conf" "${1}/packagesite.pkg" "${1}/data.pkg"
	pkg repo -q "${1}" signing_command: ${SIGNCMD}
}

# Same steps as the tools' setup_version(), with our release as the version.
set_version()
{
	WORK=${1}
	NAME=${2}
	VDIR=${WORK}/usr/local/opnsense/version
	HASH=$(cat ${VDIR}/${NAME}.hash)
	[ -f ${VDIR}/${NAME}.obsolete ] && cp ${VDIR}/${NAME}.obsolete ${TMP}/obsolete
	rm -rf ${VDIR}
	SIZE=$(tar -C ${WORK} -cf - . | wc -c | awk '{ print $1 }')
	mkdir -p ${VDIR}
	[ -f ${TMP}/obsolete ] && mv ${TMP}/obsolete ${VDIR}/${NAME}.obsolete
	echo "${SIZE}" > ${VDIR}/${NAME}.size
	echo "${HASH}" > ${VDIR}/${NAME}.hash
	echo "${ARCH}" > ${VDIR}/${NAME}.arch
	echo "${RELEASE}" > ${VDIR}/${NAME}
	echo "./var" > ${TMP}/mtree.exclude
	mtree -c -k uid,gid,mode,size,sha256digest -p ${WORK} -X ${TMP}/mtree.exclude > ${TMP}/mtree
	mv ${TMP}/mtree ${VDIR}/${NAME}.mtree
	sign_file ${VDIR}/${NAME}.mtree
	chmod 600 ${VDIR}/${NAME}.mtree*
}

repack_set()
{
	tar -C ${1} -cf - . | xz > ${2}
	sign_file ${2}
}

BASESET=$(find_one "base-*-${ARCH}-${DEVICE}.txz")
KERNELSET=$(find_one "kernel-*-${ARCH}-${DEVICE}.txz")
PACKAGESET=$(find_one "packages-*-${ARCH}.tar")

TOP=${OUT}/${ABI}/${SERIES}
TMP=$(mktemp -d /tmp/publish.XXXXXX)
# Sets keep system immutable flags when extracted; clear them before deleting.
cleanup() { chflags -R 0 ${TMP} 2>/dev/null; rm -rf ${TMP}; }
trap cleanup EXIT INT TERM
mkdir -p ${TOP}/latest ${TOP}/mono/All ${TOP}/sets

echo ">>> latest: $(basename ${PACKAGESET})"
tar -C ${TOP}/latest -xf ${PACKAGESET}
rm -f ${TOP}/latest/.*_done
if [ -n "${OPNSENSE_UPDATE_PKG}" ]; then
	[ "$(pkg query -F ${OPNSENSE_UPDATE_PKG} %v)" = "${RELEASE}" ] || { echo "ERROR: ${OPNSENSE_UPDATE_PKG} is not release ${RELEASE}"; exit 1; }
	echo ">>> latest: opnsense-update -> $(basename ${OPNSENSE_UPDATE_PKG})"
	rm -f ${TOP}/latest/All/opnsense-update-*.pkg ${TOP}/latest/Latest/opnsense-update.pkg
	cp ${OPNSENSE_UPDATE_PKG} ${TOP}/latest/All/
	ln -s ../All/$(basename ${OPNSENSE_UPDATE_PKG}) ${TOP}/latest/Latest/opnsense-update.pkg
fi
for f in ${TOP}/latest/Latest/*.pkg; do sign_file ${f}; done
sign_repo ${TOP}/latest

echo ">>> mono: $(basename ${MONOPKG}) $(basename ${SAFETYPKG})"
cp ${MONOPKG} ${SAFETYPKG} ${TOP}/mono/All/
sign_repo ${TOP}/mono

echo ">>> base: $(basename ${BASESET}) -> base-${RELEASE}-${ARCH}.txz"
mkdir ${TMP}/base
tar -C ${TMP}/base -xpf ${BASESET}
set_version ${TMP}/base base
repack_set ${TMP}/base ${TOP}/sets/base-${RELEASE}-${ARCH}.txz
chflags -R 0 ${TMP}/base
rm -rf ${TMP}/base

echo ">>> kernel: $(basename ${KERNELSET}) -> kernel-${RELEASE}-${ARCH}-${DEVICE}.txz (adds kernel.bin)"
mkdir ${TMP}/kernel
tar -C ${TMP}/kernel -xpf ${KERNELSET}
K=${TMP}/kernel/boot/kernel
# U-Boot booti image, same recipe as GATEWAY.conf; the stop hook moves it to /boot/msdos/kernel.img.
${XPREFIX}-objcopy --wildcard --strip-symbol='$[adtx]*' --output-target=binary ${K}/kernel ${TMP}/kernel.flat
{ ${XPREFIX}-nm ${K}/kernel | LC_ALL=C awk -f /usr/src/sys/tools/arm_kernel_boothdr.awk -v hdrtype=v8booti && cat ${TMP}/kernel.flat; } > ${K}/kernel.bin
rm ${TMP}/kernel.flat
set_version ${TMP}/kernel kernel
repack_set ${TMP}/kernel ${TOP}/sets/kernel-${RELEASE}-${ARCH}-${DEVICE}.txz
chflags -R 0 ${TMP}/kernel
rm -rf ${TMP}/kernel

echo ">>> done: ${OUT}"
du -sh ${TOP}/*
