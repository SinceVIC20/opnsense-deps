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


def local_addresses():
    """The router's own addresses."""
    found = set()
    for line in run(['/sbin/ifconfig', '-a']).splitlines():
        words = line.split()
        if len(words) > 1 and words[0] in ('inet', 'inet6'):
            found.add(words[1].split('%')[0])
    return found


def host_address(host):
    """Address without the port: 1.2.3.4:443 or 2001:db8::1[443]."""
    return host.split('[')[0] if '[' in host else host.rsplit(':', 1)[0]


def seconds(text):
    """pfctl's [h:]mm:ss as seconds."""
    total = 0
    for part in text.split(':'):
        total = total * 60 + int(part)
    return total


def pf_active_connections():
    """
    TCP and UDP connections routed through the firewall that carried traffic
    in the last 5 minutes. A routed connection has a state on each side, so
    the pair is counted once. Connections to the router itself are left out.
    """
    timeouts = {}
    for line in run(['/sbin/pfctl', '-st']).splitlines():
        words = line.split()
        if len(words) == 2 and words[1].endswith('s'):
            timeouts[words[0]] = int(words[1][:-1])
    local = local_addresses()
    seen = set()
    hosts, proto, timeout = None, None, 0
    for line in run(['/sbin/pfctl', '-ss', '-vv']).splitlines():
        parts = line.split()
        if line.startswith('all '):
            hosts = None
            states = parts[-1].split(':')
            if parts[1] == 'tcp' and states == ['ESTABLISHED', 'ESTABLISHED']:
                timeout = timeouts.get('tcp.established', 86400)
            elif parts[1] == 'udp' and 'NO_TRAFFIC' not in states:
                timeout = timeouts.get('udp.multiple', 60)
            else:
                continue
            # "(address)" is the address before NAT, which the other side's state uses
            hosts = []
            for word in parts[2:-1]:
                if word in ('->', '<-'):
                    continue
                if word.startswith('(') and hosts:
                    hosts[-1] = word.strip('()')
                else:
                    hosts.append(word)
            proto = parts[1]
        elif hosts and len(hosts) == 2 and 'expires in' in line:
            idle = timeout - seconds(line.split('expires in ')[1].split(',')[0])
            if idle < 300 and not any(host_address(h) in local for h in hosts):
                seen.add((proto, min(hosts), max(hosts)))
            hosts = None
    return len(seen)


def cmm_cpu_connections():
    """Connections cmm tracks that are not in hardware."""
    try:
        with open('/var/run/cmm.conn') as f:
            tracked, offloaded = (int(x) for x in f.read().split())
    except (OSError, ValueError):
        return 0
    return max(tracked - offloaded, 0)


def offload():
    """
    on/off/unavailable plus the number of connections in hardware and on the
    CPU. With offload on, cmm knows which connections stay on the CPU; without
    it, every active routed connection does.
    """
    count = run(['/sbin/sysctl', '-n', 'dev.cdx.stats.active_connections'])
    if count == '':
        return 'unavailable', 0, pf_active_connections()
    if not cmm_running():
        return 'off', 0, pf_active_connections()
    try:
        hardware = int(count)
    except ValueError:
        hardware = 0
    return 'on', hardware, cmm_cpu_connections()


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
    state, connections, cpu = offload()
    server, server_url = update_server()
    print(json.dumps({
        'release': version,
        'commit': commit,
        'offload': state,
        'connections': connections,
        'cpu_connections': cpu,
        'server': server,
        'server_url': server_url,
    }))
