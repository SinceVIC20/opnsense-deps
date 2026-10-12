#!/bin/sh
# Switches this Mono Gateway to the temporary, unofficial update server
# mono-opnsense.sincevic20.com. Run use-official-updates.sh to switch back.

set -e

SERVER=${SERVER:-https://mono-opnsense.sincevic20.com}
FPDIR=/usr/local/etc/pkg/fingerprints/OPNsense/trusted
FPFILE=${FPDIR}/mono-opnsense.sincevic20.com.20261008
REPOSCRIPT=/usr/local/opnsense/scripts/firmware/repos/MonoGateway.php

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

# The upgrade writes the new kernel to the boot partition and needs room for its sets.
mount | grep -q ' on /boot/msdos ' && [ -f /boot/msdos/kernel.img ] || { echo "The boot partition (/boot/msdos) is not mounted, so updates can't install a kernel."; echo "Nothing changed."; exit 1; }
if [ "${SERIES}" = "26.1" ]; then
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

# The safety package tests new kernels, rolls back failed upgrades and offers the upgrade.
echo "Installing the safe-upgrade package."
if ! { pkg update -f -r MonoGateway > /dev/null && pkg install -fy -r MonoGateway mono-upgrade-safety > /dev/null; }; then
	echo "Could not install the safe-upgrade package from ${SERVER}."
	echo "Updates are switched, but nothing is offered yet. Run this script again later."
	exit 1
fi
/usr/local/sbin/mono-upgrade prepare || true

echo
grep -h "url:" /usr/local/etc/pkg/repos/OPNsense.conf /usr/local/etc/pkg/repos/MonoGateway.conf
echo
echo "Previous config saved as /root/config-before-temp-updates.xml"
echo
if [ "${SERIES}" = "26.1" ] && ! grep -qw "$(mount -p | awk '$2 == "/" { print $3 }')" /usr/local/share/mono-upgrade/rootfs; then
	echo "Note: this router's disk isn't supported by the 26.7 upgrade yet, so it isn't offered."
	echo "Updates now come from ${SERVER}; the offer appears once a release supports it."
elif [ "${SERIES}" = "26.1" ]; then
	echo "Done. Check for updates (System > Firmware > Updates) now offers the upgrade to 26.7."
	echo "The upgrade restarts the router three times by itself and can take about 20 minutes."
	echo "The page can look frozen meanwhile. Don't power off until the login page is back."
	echo "If anything goes wrong, the router goes back to 26.1 by itself and the dashboard says so."
else
	echo "Done. Use System > Firmware > Updates as normal."
	echo "During the update the page can look frozen for several minutes. Don't power off;"
	echo "the router finishes, restarts by itself and comes back (up to about 10 minutes)."
fi
if [ -f /var/db/mono-upgrade/restart-needed ]; then
	echo
	echo "The router restarts in 20 seconds to finish the setup (Ctrl-C to restart later yourself)."
	sleep 20
	shutdown -r now
fi
