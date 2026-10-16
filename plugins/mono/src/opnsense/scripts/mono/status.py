#!/usr/local/bin/python3

"""
Release and offload status for the Mono Gateway dashboard widget.
"""

import json
import os
import subprocess
import xml.etree.ElementTree as ET
from urllib.parse import urlparse


def run(cmd):
    sp = subprocess.run(cmd, capture_output=True, text=True)
    return sp.stdout.strip() if sp.returncode == 0 else ''


def release():
    """Release number and the commit it was built from."""
    version = run(['/usr/local/sbin/pkg', 'query', '%v', 'mono-gateway'])
    commit = ''
    for line in run(['/usr/local/sbin/pkg', 'query', '%At=%Av', 'mono-gateway']).splitlines():
        if line.startswith('source_commit='):
            commit = line.split('=', 1)[1]
    return version, commit


def cmm_running():
    try:
        with open('/var/run/cmm.pid') as f:
            os.kill(int(f.read().strip()), 0)
        return True
    except (OSError, ValueError):
        return False


def offload():
    """on/off/unavailable plus the number of connections in hardware."""
    count = run(['/sbin/sysctl', '-n', 'dev.cdx.stats.active_connections'])
    if count == '':
        return 'unavailable', 0
    if not cmm_running():
        return 'off', 0
    try:
        return 'on', int(count)
    except ValueError:
        return 'on', 0


def update_server():
    """Host name and site address of the firmware mirror, empty for the OPNsense default."""
    try:
        node = ET.parse('/conf/config.xml').find('./system/firmware/mirror')
    except (OSError, ET.ParseError):
        return '', ''
    if node is None or not node.text:
        return '', ''
    url = urlparse(node.text.strip())
    if url.scheme not in ('http', 'https') or not url.hostname:
        return node.text.strip(), ''
    return url.hostname, '%s://%s/' % (url.scheme, url.netloc)


if __name__ == '__main__':
    version, commit = release()
    state, connections = offload()
    server, server_url = update_server()
    print(json.dumps({
        'release': version,
        'commit': commit,
        'offload': state,
        'connections': connections,
        'server': server,
        'server_url': server_url,
    }))
