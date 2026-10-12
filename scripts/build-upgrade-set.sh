#!/bin/sh
# Adds the 26.1 -> 26.7 upgrade to a published 26.7 tree: a packages set
# (needed for major upgrades) and the 26.1 repos 26.1 routers check.
# Usage: build-upgrade-set.sh <published 26.7 tree> <outdir> <release>

set -e

SRC=${1:?usage: build-upgrade-set.sh <published-www> <outdir> <release>}
OUT=${2:?usage: build-upgrade-set.sh <published-www> <outdir> <release>}
RELEASE=${3:?usage: build-upgrade-set.sh <published-www> <outdir> <release>}
ARCH=aarch64
KEYDIR=${KEYDIR:-/root/signing}
MONOPKG=${MONOPKG:-$(ls /usr/deps/dist/mono-gateway-*.pkg)}
SAFETYPKG=${SAFETYPKG:-$(ls /usr/deps/dist/mono-upgrade-safety-*.pkg)}
SIGNCMD="/usr/tools/scripts/pkg_sign.sh ${KEYDIR}/repo.pub ${KEYDIR}/repo.key"

[ -f "${MONOPKG}" ] || { echo "ERROR: expected one mono-gateway package, got: ${MONOPKG}"; exit 1; }
[ -f "${SAFETYPKG}" ] || { echo "ERROR: expected one mono-upgrade-safety package, got: ${SAFETYPKG}"; exit 1; }
[ "$(tar -xOf ${SAFETYPKG} /usr/local/share/mono-upgrade/release 2>/dev/null)" = "${RELEASE}" ] || { echo "ERROR: ${SAFETYPKG} does not carry release ${RELEASE}"; exit 1; }
[ ! -e "${OUT}" ] || { echo "ERROR: ${OUT} already exists"; exit 1; }

sign_file()
{
	sha256 -q "${1}" | ${SIGNCMD} > "${1}.sig"
}

sign_repo()
{
	rm -f "${1}/meta" "${1}/meta.conf" "${1}/packagesite.pkg" "${1}/data.pkg"
	pkg repo -q "${1}" signing_command: ${SIGNCMD}
}

TMP=$(mktemp -d /tmp/upgrade-test.XXXXXX)
trap 'rm -rf ${TMP}' EXIT INT TERM

NEW=${OUT}/FreeBSD:15:${ARCH}/26.7
OLD=${OUT}/FreeBSD:14:${ARCH}/26.1

echo ">>> copy of ${SRC}"
mkdir -p ${OUT}
cp -Rp ${SRC}/FreeBSD:15:${ARCH} ${OUT}/

# The major upgrade installs packages only from this set, so it also carries
# mono-gateway (otherwise the old FreeBSD 14 modules stay behind) and the
# safety package.  It holds
# only what a fresh install has (core and its dependencies), because the
# small root partition must fit the set twice; plugins come back afterwards
# through a normal update.
echo ">>> packages set: core + dependencies + $(basename ${MONOPKG}) + $(basename ${SAFETYPKG})"
mkdir -p ${TMP}/fp/trusted ${TMP}/repos ${TMP}/db ${TMP}/cache
cp ${KEYDIR}/fingerprint ${TMP}/fp/trusted/key
echo "OPNsense: { url: \"file://${NEW}/latest\", fingerprints: \"${TMP}/fp\", signature_type: \"fingerprints\", enabled: yes }" > ${TMP}/repos/r.conf
echo "FreeBSD: { enabled: no }" >> ${TMP}/repos/r.conf
P="pkg -o ABI=FreeBSD:15:${ARCH} -o OSVERSION=1501000 -o REPOS_DIR=${TMP}/repos -o PKG_DBDIR=${TMP}/db -o PKG_CACHEDIR=${TMP}/cache"
${P} update -q
NAMES=$(env ASSUME_ALWAYS_YES=yes ${P} install -n -r OPNsense opnsense pkg opnsense-update | sed -n 's/^[[:space:]]*\([^ ]*\): \([^ ]*\)$/\1/p' | sort -u)
mkdir -p ${TMP}/packages/All ${TMP}/packages/Latest
cp ${NEW}/latest/.abi_hint ${TMP}/packages/
for n in ${NAMES}; do
	f=$(readlink ${NEW}/latest/Latest/${n}.pkg) || { echo "ERROR: ${n} not in latest"; exit 1; }
	cp ${NEW}/latest/Latest/${f} ${TMP}/packages/All/
	ln -s ${f} ${TMP}/packages/Latest/${n}.pkg
	sign_file ${TMP}/packages/Latest/${n}.pkg
done
echo ">>> $(echo ${NAMES} | wc -w | tr -d ' ') packages + mono-gateway + mono-upgrade-safety"
cp ${MONOPKG} ${SAFETYPKG} ${TMP}/packages/All/
ln -s ../All/$(basename ${MONOPKG}) ${TMP}/packages/Latest/mono-gateway.pkg
ln -s ../All/$(basename ${SAFETYPKG}) ${TMP}/packages/Latest/mono-upgrade-safety.pkg
sign_file ${TMP}/packages/Latest/mono-gateway.pkg
sign_file ${TMP}/packages/Latest/mono-upgrade-safety.pkg
sign_repo ${TMP}/packages
tar -C ${TMP}/packages -cf ${NEW}/sets/packages-${RELEASE}-${ARCH}.tar .
sign_file ${NEW}/sets/packages-${RELEASE}-${ARCH}.tar

# 26.1 routers only offer the upgrade when their own repos answer: latest
# holds an empty package named like the core package, mono the safety package.
echo ">>> stand-in 26.1 repos"
cat > ${TMP}/manifest <<EOF
{
  "name": "opnsense",
  "origin": "opnsense/opnsense",
  "version": "26.1",
  "comment": "Stand-in so 26.1 routers can be offered the 26.7 upgrade",
  "desc": "Empty package. It only answers the 26.1 update check and is never installed.",
  "maintainer": "mono-opnsense.sincevic20.com",
  "www": "https://mono-opnsense.sincevic20.com/",
  "abi": "FreeBSD:*",
  "arch": "freebsd:*",
  "prefix": "/usr/local"
}
EOF
mkdir -p ${OLD}/latest/All ${OLD}/mono/All
pkg create -M ${TMP}/manifest -o ${OLD}/latest/All
cp ${SAFETYPKG} ${OLD}/mono/All/
sign_repo ${OLD}/latest
sign_repo ${OLD}/mono

echo ">>> done: ${OUT}"
ls -la ${NEW}/sets ${OLD}/latest/All ${OLD}/mono/All
du -sh ${OUT}/*/*
