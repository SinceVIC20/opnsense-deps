#!/bin/sh
# Puts back the kernel that was running before the last kernel update.
#   sh repair-kernel.sh check      - show what would be restored, change nothing
#   sh repair-kernel.sh rollback   - restore the previous kernel, then reboot
# If the router won't boot at all, first boot the previous kernel from U-Boot
# (see the update instructions), then run rollback.

set -e

M=/boot/msdos

[ "$(sysctl -n kern.ident)" = "MONO-GATEWAY" ] || { echo "This is not a Mono Gateway, nothing changed."; exit 1; }
[ "$(id -u)" = 0 ] || { echo "Run this as root."; exit 1; }

show()
{
	echo "Running kernel:   $(uname -v)"
	echo "kernel.img:       $(ls -l ${M}/kernel.img | awk '{ print $6, $7, $8 }')"
	if [ -f ${M}/kernel.old ]; then
		echo "kernel.old:       $(ls -l ${M}/kernel.old | awk '{ print $6, $7, $8 }')  <- will be restored"
	else
		echo "kernel.old:       missing"
	fi
	[ -d /boot/kernel.old ] && echo "/boot/kernel.old: present  <- modules will be restored" || echo "/boot/kernel.old: missing"
	[ -f ${M}/dtb/mono-gateway-dk.old ] && echo "dtb .old:         present  <- will be restored" || echo "dtb .old:         missing (dtb kept as is)"
}

case "$1" in
check)
	show
	;;
rollback)
	[ -f ${M}/kernel.old ] && [ -d /boot/kernel.old ] || { show; echo "Nothing to roll back to."; exit 1; }
	# A 26.1 kernel can't run the 26.7 base an upgrade left behind (FreeBSD 14 vs 15).
	OLDMAJOR=$(grep -a -o -m 1 'FreeBSD [0-9][0-9]*\.[0-9]*-' /boot/kernel.old/kernel | head -n 1 | sed 's/FreeBSD \([0-9]*\)\..*/\1/')
	BASEMAJOR=$(freebsd-version -u | cut -d . -f 1)
	if [ -n "${OLDMAJOR}" ] && [ "${OLDMAJOR}" != "${BASEMAJOR}" ]; then
		show
		echo "The previous kernel is FreeBSD ${OLDMAJOR}, but the installed system is FreeBSD ${BASEMAJOR}."
		echo "Rolling back would leave the router unable to start. Nothing changed."
		exit 1
	fi
	show
	echo
	echo "Restoring the previous kernel..."
	# Keep the failed one around for a look later.
	cp ${M}/kernel.img ${M}/kernel.failed
	cp ${M}/kernel.old ${M}/kernel.img
	if [ -f ${M}/dtb/mono-gateway-dk.old ]; then
		cp ${M}/dtb/mono-gateway-dk.dtb ${M}/dtb/mono-gateway-dk.failed
		cp ${M}/dtb/mono-gateway-dk.old ${M}/dtb/mono-gateway-dk.dtb
	fi
	if [ -d /boot/kernel.failed ]; then
		chflags -R 0 /boot/kernel.failed
		rm -rf /boot/kernel.failed
	fi
	mv /boot/kernel /boot/kernel.failed
	mv /boot/kernel.old /boot/kernel
	kldxref /boot/kernel
	sync
	echo "Done. The failed kernel is kept as ${M}/kernel.failed and /boot/kernel.failed."
	echo "Reboot now:  reboot"
	;;
*)
	echo "usage: sh repair-kernel.sh check | rollback"
	;;
esac
