#!/bin/sh
# Keeps a copy of the OPNsense configuration in unused eMMC space past the OPNsense partition,
# where flashing a new image leaves it alone.
#   save        save the configuration and its history
#   show        show what is saved
#   restore     put the saved configuration back, then reboot
#   from-copy   take the configuration from a raw copy of the old OPNsense partition, made in
#               Recovery Linux before a flash, then reboot

DEV=/dev/mmcsd0
AT=8192			# MiB from the start of the eMMC
SIZE=16			# MiB set aside for it
COPY=20971520		# sectors: where Recovery Linux puts the raw partition copy (10 GiB)
COPYSIZE=20971520	# sectors read as that copy (10 GiB)
TMP=/tmp/config-save.tgz

[ "$(sysctl -n kern.ident)" = "MONO-GATEWAY" ] || { echo "This is not a Mono Gateway, nothing changed."; exit 1; }
[ "$(id -u)" = 0 ] || { echo "Run this as root."; exit 1; }

saved()
{
	dd if=${DEV} bs=1m skip=${AT} count=${SIZE} 2>/dev/null | tar -tzf - 2>/dev/null | grep -qx conf/config.xml
}

show()
{
	dd if=${DEV} bs=1m skip=${AT} count=${SIZE} 2>/dev/null | tar -tvzf - conf/config.xml 2>/dev/null |
	    awk '{ print "Saved configuration from " $6, $7, $8 }'
}

# End of the last partition, in sectors.
last_sector()
{
	gpart show -p ${DEV} | awk '$3 ~ /^mmcsd0s/ { e = $1 + $2; if (e > m) m = e } END { print m + 0 }'
}

disk_sectors()
{
	echo $(( $(diskinfo ${DEV} | awk '{ print $3 }') / 512 ))
}

case "$1" in
save)
	# Only past every partition and inside the eMMC.
	LAST=$(last_sector)
	[ "${LAST}" -gt 0 ] && [ "${LAST}" -le $((AT * 2048)) ] ||
	    { echo "A partition reaches into the save area, nothing written."; exit 1; }
	[ $(((AT + SIZE) * 2048)) -le "$(disk_sectors)" ] || { echo "The eMMC is too small for the save area, nothing written."; exit 1; }
	tar -czf ${TMP} -C / conf/config.xml conf/backup || exit 1
	# A very long history doesn't fit: then the configuration alone.
	[ "$(stat -f %z ${TMP})" -lt $((SIZE * 1048576)) ] || tar -czf ${TMP} -C / conf/config.xml || exit 1
	# The eMMC is in use, so FreeBSD only allows the write with its guard lifted for a moment.
	OLD=$(sysctl -n kern.geom.debugflags)
	sysctl kern.geom.debugflags=16 > /dev/null
	dd if=${TMP} of=${DEV} bs=1m seek=${AT} conv=sync 2>/dev/null
	R=$?
	sysctl kern.geom.debugflags=${OLD} > /dev/null
	rm -f ${TMP}
	if [ ${R} = 0 ] && saved; then
		show
		echo "It stays on the eMMC when a new image is flashed."
	else
		echo "Saving failed."
		exit 1
	fi
	;;
show)
	saved && show || echo "No saved configuration found."
	;;
restore)
	saved || { echo "No saved configuration found, nothing changed."; exit 1; }
	show
	dd if=${DEV} bs=1m skip=${AT} count=${SIZE} 2>/dev/null | tar -xzf - -C / || { echo "Restoring failed."; exit 1; }
	echo "Configuration restored. Restart the router to use it: reboot"
	;;
from-copy)
	TO=${2:-/}
	[ "$(last_sector)" -le ${COPY} ] || { echo "A partition reaches into the copy area, nothing changed."; exit 1; }
	[ $((COPY + COPYSIZE)) -le "$(disk_sectors)" ] || { echo "The eMMC is too small for the copy area, nothing changed."; exit 1; }
	[ -e ${DEV}s3 ] && { echo "Partition 3 is already in use, nothing changed."; exit 1; }
	# A UFS2 filesystem has 19540119 at byte 1372 of its superblock, 64 KiB in.
	MAGIC=$(dd if=${DEV} bs=512 skip=$((COPY + 128)) count=4 2>/dev/null | od -A n -t x4 -j 1372 -N 4 | tr -d ' ')
	[ "${MAGIC}" = 19540119 ] || { echo "No copy of an OPNsense partition found, nothing changed."; exit 1; }
	# Shown as a temporary partition 3 just long enough to read it, mounted by name, read-only.
	gpart add -t freebsd -b ${COPY} -s ${COPYSIZE} -i 3 mmcsd0 > /dev/null || exit 1
	MNT=$(mktemp -d /tmp/config-copy.XXXXXX)
	R=1
	if mount -t ufs -o ro ${DEV}s3 ${MNT}; then
		if [ -f ${MNT}/conf/config.xml ]; then
			F=conf/config.xml
			[ -d ${MNT}/conf/backup ] && F="${F} conf/backup"
			tar -cf - -C ${MNT} ${F} | tar -xf - -C "${TO}" && R=0
			ls -l -D '%b %e %H:%M' ${MNT}/conf/config.xml | awk '{ print "Configuration from " $6, $7, $8 }'
		fi
		umount ${MNT}
	fi
	rmdir ${MNT}
	gpart delete -i 3 mmcsd0 > /dev/null
	[ -e ${DEV}s3 ] && echo "Partition 3 could not be removed: run gpart delete -i 3 mmcsd0 before restarting."
	if [ ${R} = 0 ]; then
		echo "Configuration copied from the old partition. Restart the router to use it: reboot"
	else
		echo "Could not read the configuration from the copy."
		exit 1
	fi
	;;
*)
	echo "Usage: $0 save | show | restore | from-copy"
	exit 1
	;;
esac
