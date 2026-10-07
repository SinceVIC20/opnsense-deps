#!/bin/sh
# Zero the free space in a GATEWAY image, then compress it with bzip2.
# Zeroed free space compresses away, and bzip2 is the format OPNsense and mono-imager expect.

set -e

IMAGESDIR=${1:?usage: compress-image.sh <imagesdir>}

set -- ${IMAGESDIR}/OPNsense-*-GATEWAY.img
if [ $# -ne 1 ] || [ ! -f "$1" ]; then
	echo "ERROR: expected exactly one OPNsense-*-GATEWAY.img in ${IMAGESDIR}"
	exit 1
fi
IMG=$1

MNT=$(mktemp -d /tmp/compress-image.XXXXXX)
MD=$(mdconfig -a -t vnode -f "${IMG}")
cleanup() {
	umount "${MNT}" 2>/dev/null || true
	mdconfig -d -u "${MD}" 2>/dev/null || true
	rmdir "${MNT}" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

# Fill each partition's free space with a zero file, then delete it.
zero_fill() {
	dd if=/dev/zero of="${MNT}/zero.fill" bs=1m 2>/dev/null || true
	sync
	rm -f "${MNT}/zero.fill"
	sync
	umount "${MNT}"
}

echo ">>> Zeroing free space in $(basename "${IMG}")"
fsck_ffs -n /dev/${MD}s2a > /dev/null
mount /dev/${MD}s2a "${MNT}"
zero_fill
fsck_ffs -n /dev/${MD}s2a > /dev/null
mount -t msdosfs /dev/${MD}s1 "${MNT}"
zero_fill

cleanup
trap - EXIT INT TERM

echo ">>> Compressing with bzip2 (takes several minutes)"
rm -f "${IMG}.bz2"
bzip2 -9 -k "${IMG}"
bzip2 -t "${IMG}.bz2"

for f in "${IMG}" "${IMG}.bz2"; do
	sha256 -r "${f}" | sed 's| .*/| |' > "${f}.sha256"
	md5 -r "${f}" | sed 's| .*/| |' > "${f}.md5"
done
