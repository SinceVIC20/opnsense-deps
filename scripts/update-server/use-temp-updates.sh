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

# The server only carries 26.7; older versions need a fresh install of the 26.7 image.
SERIES=$(/usr/local/sbin/opnsense-version -a)
[ "${SERIES}" = "26.7" ] || { echo "This router runs ${SERIES}. The temporary server only has 26.7."; echo "Install the 26.7 image first: ${SERVER}/images/"; echo "Nothing changed."; exit 1; }

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

echo
grep -h "url:" /usr/local/etc/pkg/repos/OPNsense.conf /usr/local/etc/pkg/repos/MonoGateway.conf
echo
echo "Done. Use System > Firmware > Updates as normal."
echo "During the update the page can look frozen for several minutes. Don't power off;"
echo "the router finishes, restarts by itself and comes back (up to about 10 minutes)."
echo "Previous config saved as /root/config-before-temp-updates.xml"
