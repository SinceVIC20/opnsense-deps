#!/bin/sh
# Keeps a copy of the OPNsense configuration in unused eMMC space past the OPNsense partition,
# where flashing a new image leaves it alone, and takes configurations back after a flash.
#   save        save the configuration and its history (also runs on every settings change)
#   show        show the saved copy and any copy of an old partition
#   restore     put back the newest configuration found, then reboot
#   from-copy   take the configuration from the copy of an old partition only, then reboot
# A copy of an old partition is made in Recovery Linux before the flash, with save-old-partition.sh.

DEV=/dev/mmcsd0
AT=8192			# MiB from the start of the eMMC
SIZE=16			# MiB set aside for it
COPY=20971520		# sectors: where the copy of an old partition starts (10 GiB)
COPYMAX=20971520	# sectors: the largest copy read
STATE=/var/db/mono-upgrade
ROOT=${MONO_CONFIG_ROOT:-/}	# where restore puts the configuration

[ "$(sysctl -n kern.ident)" = "MONO-GATEWAY" ] || { echo "This is not a Mono Gateway, nothing changed."; exit 1; }
[ "$(id -u)" = 0 ] || { echo "Run this as root."; exit 1; }

TMP=$(mktemp -d /tmp/config-save.XXXXXX) || exit 1
trap 'rm -rf ${TMP}' EXIT

# Which installation saved a copy: a flashed router starts with a new one, so its first automatic
# save can't replace the copy from before the flash.
install_id()
{
	[ -s ${STATE}/install-id ] || { mkdir -p ${STATE}; od -A n -N 8 -t x8 /dev/random | tr -d ' ' > ${STATE}/install-id; }
	cat ${STATE}/install-id
}

last_sector()
{
	gpart show -p ${DEV} | awk '$3 ~ /^mmcsd0s/ { e = $1 + $2; if (e > m) m = e } END { print m + 0 }'
}

disk_sectors()
{
	echo $(( $(diskinfo ${DEV} | awk '{ print $3 }') / 512 ))
}

when()
{
	stat -f '%Sm' -t '%b %e %Y %H:%M' "$1"
}

# The saved copy, unpacked into $1.
saved_get()
{
	mkdir -p "$1"
	dd if=${DEV} bs=1m skip=${AT} count=${SIZE} 2>/dev/null | tar -xzf - -C "$1" 2>/dev/null
	[ -f "$1/conf/config.xml" ]
}

# The configuration from the copy of an old partition, UFS or ZFS, unpacked into $1. It is shown as a
# temporary partition 3 just long enough to read it, read-only, then removed again.
copy_get()
{
	[ "$(last_sector)" -lt ${COPY} ] || return 1
	[ $((COPY + COPYMAX)) -le "$(disk_sectors)" ] || return 1
	[ -e ${DEV}s3 ] && { echo "Partition 3 is already in use, the copy of the old partition was not read."; return 1; }
	N=$(dd if=${DEV} bs=512 skip=$((COPY - 1)) count=1 2>/dev/null | head -c 32 | awk '$1 == "MONOCOPY1" { print $2 }')
	[ -n "${N}" ] && [ "${N}" -le ${COPYMAX} ] || N=${COPYMAX}
	if dd if=${DEV} bs=512 skip=$((COPY + 128)) count=4 2>/dev/null | od -A n -t x4 -j 1372 -N 4 | grep -q 19540119; then
		KIND=UFS
	elif dd if=${DEV} bs=1k skip=$((COPY / 2 + 16)) count=112 2>/dev/null | grep -a -q pool_guid; then
		KIND=ZFS
	else
		return 1
	fi
	gpart add -t freebsd -b ${COPY} -s ${N} -i 3 mmcsd0 > /dev/null || return 1
	MNT=${TMP}/mnt
	mkdir -p ${MNT} "$1"
	if [ ${KIND} = UFS ]; then
		mount -t ufs -o ro ${DEV}s3 ${MNT} && { take "$1"; umount ${MNT}; }
	else
		POOL=$(zdb -l ${DEV}s3 2>/dev/null | awk -F "'" '/^ *name:/ { print $2; exit }')
		if [ -n "${POOL}" ] && ! zpool list -H -o name | grep -qx "${POOL}" &&
		    zpool import -N -f -o readonly=on -R ${MNT} -d ${DEV}s3 "${POOL}" 2>/dev/null; then
			FS=$(zpool get -H -o value bootfs "${POOL}")
			[ "${FS}" = - ] && FS=${POOL}/ROOT/default
			zfs mount "${FS}" 2>/dev/null && take "$1"
			zpool export -f "${POOL}"
		fi
	fi
	gpart delete -i 3 mmcsd0 > /dev/null
	[ -e ${DEV}s3 ] && echo "Partition 3 could not be removed: run gpart delete -i 3 mmcsd0 before restarting."
	[ -f "$1/conf/config.xml" ]
}

take()
{
	F=conf/config.xml
	[ -d ${MNT}/conf/backup ] && F="${F} conf/backup"
	[ -f ${MNT}/conf/config.xml ] && tar -cf - -C ${MNT} ${F} | tar -xf - -C "$1"
}

# Puts the configuration unpacked in $1 in place.
put()
{
	tar -cf - -C "$1" conf | tar -xf - -C "${ROOT}" || { echo "Restoring failed."; exit 1; }
	# Carry on as the installation that saved the copy, so settings saves update it again.
	[ -s "${TMP}/saved/mono-install-id" ] && cp "${TMP}/saved/mono-install-id" ${STATE}/install-id
	echo "Restart the router to use it: reboot"
}

case "$1" in
save)
	LAST=$(last_sector)
	[ "${LAST}" -gt 0 ] && [ "${LAST}" -le $((AT * 2048)) ] ||
	    { echo "A partition reaches into the save area, nothing written."; exit 1; }
	[ $(((AT + SIZE) * 2048)) -le "$(disk_sectors)" ] || { echo "The eMMC is too small for the save area, nothing written."; exit 1; }
	ME=$(install_id)
	if [ "$2" = auto ] && saved_get ${TMP}/saved && [ -s ${TMP}/saved/mono-install-id ] &&
	    [ "$(cat ${TMP}/saved/mono-install-id)" != "${ME}" ]; then
		echo "The saved copy is from before this router was flashed: kept until restored (restore) or replaced (save)."
		exit 0
	fi
	echo "${ME}" > ${TMP}/mono-install-id
	tar -czf ${TMP}/new.tgz -C / conf/config.xml conf/backup -C ${TMP} mono-install-id || exit 1
	# A very long history doesn't fit: then the configuration alone.
	[ "$(stat -f %z ${TMP}/new.tgz)" -lt $((SIZE * 1048576)) ] ||
	    tar -czf ${TMP}/new.tgz -C / conf/config.xml -C ${TMP} mono-install-id || exit 1
	# The eMMC is in use, so FreeBSD only allows the write with its guard lifted for a moment.
	OLD=$(sysctl -n kern.geom.debugflags)
	sysctl kern.geom.debugflags=16 > /dev/null
	dd if=${TMP}/new.tgz of=${DEV} bs=1m seek=${AT} conv=sync 2>/dev/null
	R=$?
	sysctl kern.geom.debugflags=${OLD} > /dev/null
	if [ ${R} = 0 ] && saved_get ${TMP}/check; then
		echo "Saved configuration from $(when ${TMP}/check/conf/config.xml). It stays on the eMMC when a new image is flashed."
	else
		echo "Saving failed."
		exit 1
	fi
	;;
show)
	if saved_get ${TMP}/saved; then
		echo "Saved copy: configuration from $(when ${TMP}/saved/conf/config.xml)"
	else
		echo "Saved copy: none"
	fi
	if copy_get ${TMP}/copy; then
		echo "Copy of the old ${KIND} partition: configuration from $(when ${TMP}/copy/conf/config.xml)"
	else
		echo "Copy of an old partition: none"
	fi
	;;
restore)
	A=; B=
	saved_get ${TMP}/saved && A=$(stat -f %m ${TMP}/saved/conf/config.xml)
	copy_get ${TMP}/copy && B=$(stat -f %m ${TMP}/copy/conf/config.xml)
	if [ -z "${A}${B}" ]; then
		echo "No saved configuration found, nothing changed."
		exit 1
	elif [ -n "${A}" ] && { [ -z "${B}" ] || [ "${A}" -ge "${B}" ]; }; then
		echo "Restoring the saved copy: configuration from $(when ${TMP}/saved/conf/config.xml)"
		put ${TMP}/saved
	else
		echo "Restoring from the copy of the old ${KIND} partition: configuration from $(when ${TMP}/copy/conf/config.xml)"
		put ${TMP}/copy
	fi
	;;
from-copy)
	copy_get ${TMP}/copy || { echo "No copy of an old partition found, nothing changed."; exit 1; }
	echo "Restoring from the copy of the old ${KIND} partition: configuration from $(when ${TMP}/copy/conf/config.xml)"
	put ${TMP}/copy
	;;
*)
	echo "Usage: $0 save | show | restore | from-copy"
	exit 1
	;;
esac
