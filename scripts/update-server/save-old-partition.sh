#!/bin/sh
# For Mono's Recovery Linux, before flashing a new OPNsense image: copies the old OPNsense partition,
# as it is, and the start-up firmware area (the first 32 MiB) into unused eMMC space that the flash
# leaves alone. After the flash, OPNsense takes the configuration out with: mono-config-save restore
#   curl -s http://10.0.0.1:8000/save-old-partition.sh | sh

DISK=mmcblk0
PART=mmcblk0p2
FW=18874368	# sectors: the copy of the first 32 MiB goes 9 GiB into the eMMC
AT=20971520	# sectors: the partition copy starts 10 GiB into the eMMC
MARK=$((AT - 1))	# the sector just before it says how big the copy is
R=0

say() { echo "$*"; }

DISKSIZE=$(cat /sys/class/block/${DISK}/size)

# What a filesystem looks like from its first bytes: UFS has 19540119 at byte 1372 of the superblock
# (64 KiB in), ZFS has its pool details 16 KiB in.
kind()
{
	if dd if=/dev/${DISK} bs=512 skip=$(($1 + 128)) count=4 2>/dev/null | od -A n -t x4 -j 1372 -N 4 | grep -q 19540119; then
		echo UFS
	elif dd if=/dev/${DISK} bs=1024 skip=$(($1 / 2 + 16)) count=112 2>/dev/null | grep -a -q pool_guid; then
		echo ZFS
	else
		echo unknown
	fi
}

copy_partition()
{
	[ -e /sys/class/block/${PART}/start ] || { say "No OPNsense partition found (${PART}), nothing copied."; return 1; }
	START=$(cat /sys/class/block/${PART}/start)
	SIZE=$(cat /sys/class/block/${PART}/size)
	say "OPNsense partition: $((SIZE / 2048)) MiB, $(kind ${START})"
	if [ $((START + SIZE)) -gt ${MARK} ] || [ $((AT + SIZE)) -gt ${DISKSIZE} ]; then
		say "The OPNsense partition fills the eMMC, so there is no room for a copy. Nothing copied:"
		say "flashing a new image will erase its configuration."
		return 1
	fi
	OLD=$(kind ${AT})
	[ "${OLD}" = unknown ] || say "An earlier copy (${OLD}) is there already, replacing it with a fresh one."
	say "Copying $((SIZE / 2048)) MiB, about 3 minutes..."
	dd if=/dev/${PART} of=/dev/${DISK} bs=1M seek=$((AT / 2048)) 2>/dev/null || { say "Copying failed."; return 1; }
	printf 'MONOCOPY1 %s\n' ${SIZE} > /tmp/mono-copy-mark
	dd if=/dev/zero bs=1 count=$((512 - $(wc -c < /tmp/mono-copy-mark))) 2>/dev/null >> /tmp/mono-copy-mark
	dd if=/tmp/mono-copy-mark of=/dev/${DISK} bs=512 seek=${MARK} 2>/dev/null
	rm -f /tmp/mono-copy-mark
	sync
	# The start and the end of the copy must match the partition.
	LAST=$((SIZE / 2048 - 16))
	A=$(dd if=/dev/${PART} bs=1M count=16 2>/dev/null | md5sum | cut -c1-32)
	B=$(dd if=/dev/${DISK} bs=1M skip=$((AT / 2048)) count=16 2>/dev/null | md5sum | cut -c1-32)
	C=$(dd if=/dev/${PART} bs=1M skip=${LAST} count=16 2>/dev/null | md5sum | cut -c1-32)
	D=$(dd if=/dev/${DISK} bs=1M skip=$((AT / 2048 + LAST)) count=16 2>/dev/null | md5sum | cut -c1-32)
	if [ "${A}" = "${B}" ] && [ "${C}" = "${D}" ]; then
		say "Copy done and checked."
	else
		say "The copy does not match the partition, do not rely on it."
		return 1
	fi
}

# After the partition copy, since a partition reaching past 9 GiB would be overwritten by it.
copy_firmware()
{
	[ $((FW + 65536)) -le ${DISKSIZE} ] || { say "The eMMC is too small for a copy of the start-up firmware area."; return 1; }
	dd if=/dev/${DISK} of=/dev/${DISK} bs=1M count=32 seek=$((FW / 2048)) 2>/dev/null && sync
	A=$(dd if=/dev/${DISK} bs=1M count=32 2>/dev/null | md5sum | cut -c1-32)
	B=$(dd if=/dev/${DISK} bs=1M skip=$((FW / 2048)) count=32 2>/dev/null | md5sum | cut -c1-32)
	if [ "${A}" = "${B}" ]; then
		say "Start-up firmware area saved and checked."
	else
		say "The start-up firmware area copy does not match, do not flash."
		return 1
	fi
}

copy_partition || R=1
copy_firmware || R=2
[ ${R} = 2 ] || say "Flash the new image now; afterwards, at the OPNsense shell: mono-config-save restore"
exit ${R}
