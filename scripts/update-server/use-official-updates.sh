#!/bin/sh
# Switches this Mono Gateway back to Mono's official update server
# opnsense.mono.si and removes the temporary server's key.

set -e

OFFICIAL=https://opnsense.mono.si
FPDIR=/usr/local/etc/pkg/fingerprints/OPNsense/trusted
REPOSCRIPT=/usr/local/opnsense/scripts/firmware/repos/MonoGateway.php

[ "$(sysctl -n kern.ident)" = "MONO-GATEWAY" ] || { echo "This is not a Mono Gateway, nothing changed."; exit 1; }
[ "$(id -u)" = 0 ] || { echo "Run this as root."; exit 1; }

echo "Switching updates back to ${OFFICIAL}."
cp /conf/config.xml /root/config-before-official-updates.xml

# Stop trusting the temporary server's key.
rm -f ${FPDIR}/mono-opnsense.sincevic20.com.*
rm -f ${REPOSCRIPT}

# Mono's own keys, from Mono's opnsense-core patch.
[ -f ${FPDIR}/opnsense.mono.si.20260305 ] || cat > ${FPDIR}/opnsense.mono.si.20260305 <<'EOF'
function: "sha256"
fingerprint: "ed8c6cdf38383853b0d6f4c6bf70efa1793d76a57e78317a53653404bcabd931"
EOF
[ -f ${FPDIR}/opnsense.mono.si.20260716 ] || cat > ${FPDIR}/opnsense.mono.si.20260716 <<'EOF'
function: "sha256"
fingerprint: "8da295f8c3c4bc0817ac630e00c284fb2354f4fd27af4b72c58105186ff425f3"
EOF

# Mono's MonoGateway repo, as in Mono's patch.
cat > /usr/local/etc/pkg/repos/MonoGateway.conf <<EOF
MonoGateway: {
  fingerprints: "/usr/local/etc/pkg/fingerprints/OPNsense",
  url: "${OFFICIAL}/\${ABI}/$(opnsense-version -a)/mono",
  signature_type: "fingerprints",
  priority: 11,
  enabled: yes
}
EOF

# Same setting as System > Firmware > Settings > Mirror.
cat > /tmp/set-mirror.php <<'EOF'
<?php
require_once '/usr/local/opnsense/mvc/script/load_phalcon.php';
$fw = new \OPNsense\Core\Firmware();
$fw->mirror = $argv[1];
$fw->serializeToConfig();
\OPNsense\Core\Config::getInstance()->save();
EOF
php /tmp/set-mirror.php "${OFFICIAL}"
rm /tmp/set-mirror.php
configctl firmware configure > /dev/null

echo
grep -h "url:" /usr/local/etc/pkg/repos/OPNsense.conf /usr/local/etc/pkg/repos/MonoGateway.conf
echo
echo "Done. Updates now come from Mono's official server."
echo "Note: Mono's builds only include fixes Mono has merged."
echo "Previous config saved as /root/config-before-official-updates.xml"
