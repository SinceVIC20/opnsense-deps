#!/bin/sh
# Hardware and CPU connection counts for the Connections gauge, cheap enough
# to run every few seconds. Without offload, status.py counts the CPU ones.

HW=$(/sbin/sysctl -n dev.cdx.stats.active_connections 2>/dev/null)
PID=$(cat /var/run/cmm.pid 2>/dev/null)
if [ -z "${HW}" ] || [ -z "${PID}" ] || ! kill -0 "${PID}" 2>/dev/null; then
	exec /usr/local/opnsense/scripts/mono/status.py connections
fi

# Same as status.py: retry + kept + other are on the CPU.
CPU=$(awk '{for (i = 1; i < NF; i += 2) if ($i == "retry" || $i == "kept" || $i == "other") n += $(i + 1)} END {print n + 0}' /var/run/cmm.conn 2>/dev/null)
echo "{\"offload\": \"on\", \"connections\": ${HW}, \"cpu_connections\": ${CPU:-0}}"
