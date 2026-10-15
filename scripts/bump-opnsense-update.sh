#!/bin/sh
# Repackage opnsense-update with a new release, so routers fetch that
# release's base and kernel sets.  Only the RELEASE line and the package
# version change.
# Usage: bump-opnsense-update.sh <opnsense-update-X.pkg> <release> <outdir>
#   e.g. bump-opnsense-update.sh opnsense-update-26.7.2.pkg 26.7.7_2 /tmp/out

set -e

PKGFILE=${1:?usage: bump-opnsense-update.sh <pkg> <release> <outdir>}
RELEASE=${2:?usage: bump-opnsense-update.sh <pkg> <release> <outdir>}
OUT=${3:?usage: bump-opnsense-update.sh <pkg> <release> <outdir>}

OLD=$(pkg query -F "${PKGFILE}" %v)
[ "$(pkg query -F "${PKGFILE}" %n)" = "opnsense-update" ] || { echo "ERROR: not an opnsense-update package"; exit 1; }

TMP=$(mktemp -d /tmp/bump.XXXXXX)
trap 'rm -rf ${TMP}' EXIT INT TERM
mkdir ${TMP}/root

tar -xpf "${PKGFILE}" -C ${TMP}/root 2>/dev/null
sed "s/\"version\":\"${OLD}\"/\"version\":\"${RELEASE}\"/" ${TMP}/root/+MANIFEST > ${TMP}/manifest
rm ${TMP}/root/+MANIFEST ${TMP}/root/+COMPACT_MANIFEST

SCRIPT=${TMP}/root/usr/local/sbin/opnsense-update
grep -q "^RELEASE=\"${OLD}\"$" ${SCRIPT} || { echo "ERROR: RELEASE=\"${OLD}\" not found"; exit 1; }
sed -i '' "s/^RELEASE=\"${OLD}\"$/RELEASE=\"${RELEASE}\"/" ${SCRIPT}

mkdir -p "${OUT}"
pkg create -M ${TMP}/manifest -r ${TMP}/root -o "${OUT}"
echo ">>> ${OUT}/opnsense-update-${RELEASE}.pkg (was ${OLD})"
