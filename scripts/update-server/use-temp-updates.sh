#!/bin/sh
# Switches this Mono Gateway to the temporary, unofficial update server
# mono-opnsense.sincevic20.com. Run use-official-updates.sh to switch back.

set -e

SERVER=${SERVER:-https://mono-opnsense.sincevic20.com}
FPDIR=/usr/local/etc/pkg/fingerprints/OPNsense/trusted
FPFILE=${FPDIR}/mono-opnsense.sincevic20.com.20261008
REPOSCRIPT=/usr/local/opnsense/scripts/firmware/repos/MonoGateway.php
KERNELHOOK=/usr/local/etc/rc.syshook.d/stop/20-kernel-update
# The 26.7 release a 26.1 router upgrades to.
UPGRADE_RELEASE=26.7.2_2

[ "$(sysctl -n kern.ident)" = "MONO-GATEWAY" ] || { echo "This is not a Mono Gateway, nothing changed."; exit 1; }
[ "$(id -u)" = 0 ] || { echo "Run this as root."; exit 1; }

# The server carries 26.7, plus the upgrade from 26.1; older versions need a fresh install.
SERIES=$(/usr/local/sbin/opnsense-version -a)
case "${SERIES}" in
26.7|26.1)
	;;
*)
	echo "This router runs ${SERIES}. The temporary server only has 26.7 and the upgrade from 26.1."
	echo "Install the 26.7 image first: ${SERVER}/images/"
	echo "Nothing changed."
	exit 1
	;;
esac

if [ "${SERIES}" = "26.1" ]; then
	# The upgrade writes the new kernel to the boot partition and needs room for its sets.
	mount | grep -q ' on /boot/msdos ' && [ -f /boot/msdos/kernel.img ] || { echo "The boot partition (/boot/msdos) is not mounted, so the upgrade can't install its kernel."; echo "Nothing changed."; exit 1; }
	FREE=$(df -k / | awk 'NR == 2 { print $4 }')
	[ "${FREE}" -ge 1048576 ] || { echo "The upgrade needs 1 GB free on /, this router has $((FREE / 1024)) MB."; echo "Nothing changed."; exit 1; }
fi

echo "Switching updates to ${SERVER} (unofficial, temporary)."
cp /conf/config.xml /root/config-before-temp-updates.xml

# Trust the key the temporary server signs with.
mkdir -p ${FPDIR}
cat > ${FPFILE} <<'EOF'
function: "sha256"
fingerprint: "b871c4601616f1c1cd84ca4b14a23be4018733a8b271152c6f388c2ab8c1b85d"
EOF

# Keeps the MonoGateway repo on the same server as the firmware mirror.
cat > ${REPOSCRIPT} <<'EOF'
#!/usr/local/bin/php
<?php
// Added by use-temp-updates.sh: points the MonoGateway repo at the firmware mirror.
require_once('script/load_phalcon.php');
$mirror = rtrim((string)OPNsense\Core\Config::getInstance()->object()->system->firmware->mirror, '/');
$series = trim(shell_exec('/usr/local/sbin/opnsense-version -a'));
$enabled = $mirror != '' ? 'yes' : 'no';
file_put_contents('/usr/local/etc/pkg/repos/MonoGateway.conf', "MonoGateway: {
  fingerprints: \"/usr/local/etc/pkg/fingerprints/OPNsense\",
  url: \"{$mirror}/\${ABI}/{$series}/mono\",
  signature_type: \"fingerprints\",
  priority: 11,
  enabled: {$enabled}
}
");
EOF
chmod 755 ${REPOSCRIPT}

# Same setting as System > Firmware > Settings > Mirror.
cat > /tmp/set-mirror.php <<'EOF'
<?php
require_once '/usr/local/opnsense/mvc/script/load_phalcon.php';
$fw = new \OPNsense\Core\Firmware();
$fw->mirror = $argv[1];
$fw->serializeToConfig();
\OPNsense\Core\Config::getInstance()->save();
EOF
php /tmp/set-mirror.php "${SERVER}"
rm /tmp/set-mirror.php
configctl firmware configure > /dev/null

if [ "${SERIES}" = "26.1" ]; then
	# Tells Check for updates to offer the 26.7 upgrade, as OPNsense's own 26.1 updates would.
	sed -i '' '/^UPGRADE_/d' /usr/local/etc/opnsense-update.conf
	cat >> /usr/local/etc/opnsense-update.conf <<EOF
UPGRADE_RELEASE="${UPGRADE_RELEASE}"
UPGRADE_HINT="FreeBSD:15:aarch64/26.7"
EOF
	# Early 26.1 images lack the hook that moves a new kernel to the boot partition.
	if [ ! -f ${KERNELHOOK} ]; then
		mkdir -p $(dirname ${KERNELHOOK})
		cat > ${KERNELHOOK} <<'EOF'
#!/bin/sh

# move kernel and copy Gateway dtb to FAT partition after kernel update
if [ -f /boot/kernel/kernel.bin ]; then
    INSTALLED_KERNEL=$(cat /usr/local/opnsense/version/kernel)
    if [ "${INSTALLED_KERNEL}" != "26.1.3" ]; then
        cp /boot/msdos/kernel.img /boot/msdos/kernel.old
        mv /boot/kernel/kernel.bin /boot/msdos/kernel.img
        cp /boot/msdos/dtb/mono-gateway-dk.dtb /boot/msdos/dtb/mono-gateway-dk.old
        cp /boot/dtb/freescale/mono-gateway-dk.dtb /boot/msdos/dtb/
    fi
fi
EOF
		chmod 755 ${KERNELHOOK}
	fi
fi

echo
grep -h "url:" /usr/local/etc/pkg/repos/OPNsense.conf /usr/local/etc/pkg/repos/MonoGateway.conf
echo
if [ "${SERIES}" = "26.1" ]; then
	echo "Done. Use System > Firmware > Updates > Check for updates: it offers the upgrade to 26.7."
	echo "The upgrade restarts the router three times by itself and can take about 20 minutes."
	echo "The page can look frozen meanwhile. Don't power off until the login page is back."
	echo "Afterwards, run Check for updates once more to bring back any plugins."
else
	echo "Done. Use System > Firmware > Updates as normal."
	echo "During the update the page can look frozen for several minutes. Don't power off;"
	echo "the router finishes, restarts by itself and comes back (up to about 10 minutes)."
fi
echo "Previous config saved as /root/config-before-temp-updates.xml"
