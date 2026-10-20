#!/usr/local/bin/python3

"""
SFP/SFP+ module details for System > Diagnostics > SFP+.

Decodes the module memory (SFF-8472 pages A0h and A2h, codes from SFF-8024)
that the dtsec driver reads live through dev.dtsec.N.sfp_eeprom.
"""

import json
import math
import re
import glob
import os
import struct
import subprocess
import sys
import time
import xml.etree.ElementTree as ET

# Mono Gateway cages, seen from the front.
CAGES = {'dtsec3': 'Left SFP+', 'dtsec4': 'Right SFP+'}

IDENTIFIERS = {
    0x00: 'Unknown', 0x01: 'GBIC', 0x02: 'Soldered module', 0x03: 'SFP/SFP+/SFP28',
    0x0B: 'DWDM-SFP/SFP+', 0x0C: 'QSFP', 0x0D: 'QSFP+', 0x11: 'QSFP28',
}

CONNECTORS = {
    0x00: 'Unknown', 0x01: 'SC', 0x02: 'Fibre Channel style 1 copper', 0x03: 'Fibre Channel style 2 copper',
    0x04: 'BNC/TNC', 0x05: 'Fibre Channel coax', 0x06: 'Fiber Jack', 0x07: 'LC', 0x08: 'MT-RJ',
    0x09: 'MU', 0x0A: 'SG', 0x0B: 'Optical pigtail', 0x0C: 'MPO 1x12', 0x0D: 'MPO 2x16',
    0x20: 'HSSDC II', 0x21: 'Copper pigtail', 0x22: 'RJ45', 0x23: 'No separable connector',
    0x24: 'MXC 2x16', 0x25: 'CS optical', 0x26: 'SN optical', 0x27: 'MPO 2x12', 0x28: 'MPO 1x16',
}

ENCODINGS = {
    0x00: 'Unspecified', 0x01: '8B/10B', 0x02: '4B/5B', 0x03: 'NRZ', 0x04: 'Manchester',
    0x05: 'SONET scrambled', 0x06: '64B/66B', 0x07: '256B/257B', 0x08: 'PAM4',
}

# SFF-8024 extended compliance codes (A0h byte 36), the ones seen on SFP ports.
EXT_COMPLIANCE = {
    0x01: '100G AOC or 25GAUI C2M AOC', 0x02: '100GBASE-SR4 or 25GBASE-SR', 0x03: '100GBASE-LR4 or 25GBASE-LR',
    0x04: '100GBASE-ER4 or 25GBASE-ER', 0x08: '100G ACC or 25GAUI C2M ACC', 0x0B: '100GBASE-CR4 or 25GBASE-CR CA-L',
    0x0C: '25GBASE-CR CA-S', 0x0D: '25GBASE-CR CA-N', 0x16: '10GBASE-T with SFI', 0x18: '100G AOC',
    0x1C: '10GBASE-T short reach', 0x1D: '5GBASE-T', 0x1E: '2.5GBASE-T',
}

# A0h bytes 3-10: (byte, bit, name)
COMPLIANCE_BITS = [
    (3, 7, '10GBASE-ER'), (3, 6, '10GBASE-LRM'), (3, 5, '10GBASE-LR'), (3, 4, '10GBASE-SR'),
    (3, 3, '1X SX (InfiniBand)'), (3, 2, '1X LX (InfiniBand)'), (3, 1, '1X copper active (InfiniBand)'),
    (3, 0, '1X copper passive (InfiniBand)'),
    (6, 7, 'BASE-PX'), (6, 6, 'BASE-BX10'), (6, 5, '100BASE-FX'), (6, 4, '100BASE-LX/LX10'),
    (6, 3, '1000BASE-T'), (6, 2, '1000BASE-CX'), (6, 1, '1000BASE-LX'), (6, 0, '1000BASE-SX'),
    (8, 3, 'Active cable'), (8, 2, 'Passive cable'),
]

SFF8472_REVS = {
    0x00: 'not stated', 0x01: '9.3', 0x02: '9.5', 0x03: '10.2', 0x04: '10.4', 0x05: '11.0',
    0x06: '11.3', 0x07: '11.4', 0x08: '12.3', 0x09: '12.4',
}

# A2h byte 112/113 (alarms) and 116/117 (warnings): (offset in the pair, bit, name)
FLAG_BITS = [
    (0, 7, 'temperature high'), (0, 6, 'temperature low'), (0, 5, 'supply voltage high'),
    (0, 4, 'supply voltage low'), (0, 3, 'laser bias high'), (0, 2, 'laser bias low'),
    (0, 1, 'TX power high'), (0, 0, 'TX power low'), (1, 7, 'RX power high'), (1, 6, 'RX power low'),
]


def run(cmd):
    sp = subprocess.run(cmd, capture_output=True, text=True)
    return sp.stdout.strip() if sp.returncode == 0 else ''


def text(buf, lo, hi):
    return bytes(buf[lo:hi]).decode('ascii', 'replace').strip(' \x00')


def u16(buf, off):
    return (buf[off] << 8) | buf[off + 1]


def s16(buf, off):
    v = u16(buf, off)
    return v - 0x10000 if v & 0x8000 else v


def checksum_ok(buf, lo, hi, at):
    return (sum(buf[lo:hi + 1]) & 0xff) == buf[at]


def dbm(uw):
    return round(10 * math.log10(uw / 1000.0), 2) if uw and uw > 0 else None


def interface_names():
    """dtsecN -> OPNsense interface description, e.g. WAN."""
    names = {}
    try:
        root = ET.parse('/conf/config.xml').getroot()
        ifaces = root.find('interfaces')
        for node in (ifaces if ifaces is not None else []):
            dev = node.findtext('if', '')
            names.setdefault(dev, node.findtext('descr') or node.tag.upper())
    except (ET.ParseError, OSError):
        pass
    return names


def sysctl(name):
    return run(['/sbin/sysctl', '-n', name])


def ports():
    found = []
    for line in run(['/sbin/sysctl', '-N', 'dev.dtsec']).splitlines():
        m = re.match(r'dev\.dtsec\.(\d+)\.sfp_eeprom$', line)
        if m:
            found.append(int(m.group(1)))
    return sorted(found)


def link_status(dev):
    out = run(['/sbin/ifconfig', dev])
    media = re.search(r'^\s*media: (.*)$', out, re.M)
    status = re.search(r'^\s*status: (.*)$', out, re.M)
    return (media.group(1) if media else '', status.group(1) if status else '')


def expected_rx_wavelength(tx, bidi, pn):
    """The far end's transmit wavelength a BiDi module expects. Always a guess."""
    if not bidi or not tx:
        return None
    for nm in (1270, 1310, 1330, 1490, 1550, 1570):
        if nm != tx and str(nm) in pn:
            return {'nm': nm, 'basis': 'part number'}
    if tx == 1310:
        if re.search(r'BX10?-?U\b|-U$', pn):
            return {'nm': 1490, 'basis': '1000BASE-BX10-U standard pair'}
        return {'nm': None, 'choices': [1490, 1550], 'basis': 'usual pairs for a 1310 nm BiDi module'}
    if tx in (1490, 1550):
        return {'nm': 1310, 'basis': 'usual pair'}
    if tx == 1270:
        return {'nm': 1330, 'basis': 'usual pair'}
    if tx == 1330:
        return {'nm': 1270, 'basis': 'usual pair'}
    return None


def decode_a0(a0):
    info = {
        'identifier': IDENTIFIERS.get(a0[0], 'code 0x%02x' % a0[0]),
        'connector': CONNECTORS.get(a0[2], 'code 0x%02x' % a0[2]),
        'encoding': ENCODINGS.get(a0[11], 'code 0x%02x' % a0[11]),
        'vendor': text(a0, 20, 36),
        'vendor_oui': '%02x:%02x:%02x' % (a0[37], a0[38], a0[39]),
        'part_number': text(a0, 40, 56),
        'revision': text(a0, 56, 60),
        'serial': text(a0, 68, 84),
        'date_code': '',
        'compliance': [],
        'checksums_ok': checksum_ok(a0, 0, 62, 63) and checksum_ok(a0, 64, 94, 95),
        'sff8472_rev': SFF8472_REVS.get(a0[94], 'code 0x%02x' % a0[94]),
    }
    d = text(a0, 84, 92)
    if re.match(r'\d{6}', d):
        info['date_code'] = '20%s-%s-%s' % (d[0:2], d[2:4], d[4:6]) + (' lot %s' % d[6:].strip() if d[6:].strip() else '')

    for byte, bit, name in COMPLIANCE_BITS:
        if a0[byte] & (1 << bit):
            info['compliance'].append(name)
    if a0[36] in EXT_COMPLIANCE:
        info['compliance'].append(EXT_COMPLIANCE[a0[36]])
    rj45 = a0[2] == 0x22
    if rj45:
        # Copper RJ45 modules often claim a cable type; the rate says which BASE-T it is.
        info['compliance'] = [c for c in info['compliance'] if not c.endswith(' cable')]
        rate = a0[12] * 100 if a0[12] != 0xff else a0[66] * 250
        if not any('BASE-T' in c for c in info['compliance']):
            info['compliance'].append('%s copper (RJ45)' % ('10GBASE-T' if rate >= 10000 else
                                      '5GBASE-T' if rate >= 5000 else '2.5GBASE-T' if rate >= 2500 else '1000BASE-T'))

    br = a0[12] * 100 if a0[12] != 0xff else a0[66] * 250
    info['nominal_rate_mbd'] = br or None

    cable = bool(a0[8] & 0x0c) and not rj45
    info['cable'] = 'active' if cable and a0[8] & 0x08 else ('passive' if cable else None)
    info['optical'] = not cable and a0[2] not in (0x21, 0x22, 0x23)
    lengths = []
    if a0[14]:
        lengths.append('%d km single-mode' % a0[14])
    elif a0[15]:
        lengths.append('%d m single-mode' % (a0[15] * 100))
    if a0[16]:
        lengths.append('%d m OM2' % (a0[16] * 10))
    if a0[17]:
        lengths.append('%d m OM1' % (a0[17] * 10))
    if a0[18] and not rj45:
        lengths.append(('%d m cable' % a0[18]) if cable or a0[2] in (0x21, 0x22, 0x23) else ('%d m OM4' % (a0[18] * 10)))
    if a0[19]:
        lengths.append('%d m OM3' % (a0[19] * 10))
    info['reach'] = lengths

    # Bytes 60-61 are the laser wavelength, except on cables where they hold cable compliance bits.
    info['wavelength_nm'] = (u16(a0, 60) or None) if info['optical'] else None
    pn = info['part_number'].upper()
    info['bidi'] = bool(a0[6] & 0x40) or bool(re.search(r'BX|BIDI', pn))
    info['rx_wavelength'] = expected_rx_wavelength(info['wavelength_nm'], info['bidi'], pn)

    options = a0[65]
    info['options'] = {
        'tx_disable': bool(options & 0x10), 'tx_fault': bool(options & 0x08),
        'rx_los': bool(options & 0x02), 'rx_los_inverted': bool(options & 0x04),
        'rate_select': bool(options & 0x20),
    }
    diag = a0[92]
    info['ddm'] = {
        'implemented': bool(diag & 0x40),
        'calibration': 'external' if diag & 0x10 else ('internal' if diag & 0x20 else 'unknown'),
        'rx_power_type': 'average' if diag & 0x08 else 'OMA',
        'flags_implemented': bool(a0[93] & 0x80),
    }
    return info


def ext_cal(a2):
    """SFF-8472 external calibration constants."""
    rx = [struct.unpack('>f', bytes(a2[56 + 4 * i:60 + 4 * i]))[0] for i in range(5)]
    slope = lambda off: u16(a2, off) / 256.0
    return {
        'rx': rx[::-1],                       # Rx_PWR(0) .. Rx_PWR(4)
        'bias': (slope(76), s16(a2, 78)),
        'tx': (slope(80), s16(a2, 82)),
        'temp': (slope(84), s16(a2, 86)),
        'vcc': (slope(88), s16(a2, 90)),
    }


def convert(kind, raw, cal):
    """Raw A2h value -> units. Temperature is signed, the rest unsigned."""
    if cal and kind != 'rx':
        slope, offset = cal[kind]
        raw = raw * slope + offset
    elif cal and kind == 'rx':
        raw = sum(c * raw ** i for i, c in enumerate(cal['rx']))
    if kind == 'temp':
        return round(raw / 256.0, 2)          # degrees C
    if kind == 'vcc':
        return round(raw / 10000.0, 3)        # volts
    if kind == 'bias':
        return round(raw * 0.002, 2)          # mA
    return round(raw * 0.1, 1)                # microwatts


def decode_a2(a2, ddm):
    cal = ext_cal(a2) if ddm['calibration'] == 'external' else None
    fields = [('temp', 96, 0), ('vcc', 98, 8), ('bias', 100, 16), ('tx', 102, 24), ('rx', 104, 32)]
    values = {}
    for kind, at, th in fields:
        raw = s16(a2, at) if kind == 'temp' else u16(a2, at)
        rawth = [(s16(a2, th + 2 * i) if kind == 'temp' else u16(a2, th + 2 * i)) for i in range(4)]
        value = convert(kind, raw, cal)
        limits = [convert(kind, r, cal) for r in rawth]
        entry = {'value': value, 'high_alarm': limits[0], 'low_alarm': limits[1],
                 'high_warning': limits[2], 'low_warning': limits[3]}
        if kind in ('tx', 'rx'):
            entry['dbm'] = dbm(value)
            for k in ('high_alarm', 'low_alarm', 'high_warning', 'low_warning'):
                entry[k + '_dbm'] = dbm(entry[k])
        state = 'ok'
        if not any(rawth) and not raw:
            state = 'unused'                  # field not used by this module
        elif value > limits[0] or value < limits[1]:
            state = 'alarm'
        elif value > limits[2] or value < limits[3]:
            state = 'warning'
        entry['state'] = state
        values[kind] = entry

    st = a2[110]
    status = {
        'tx_disable': bool(st & 0x80), 'soft_tx_disable': bool(st & 0x40),
        'rate_select': bool(st & 0x10), 'tx_fault': bool(st & 0x04),
        'rx_los': bool(st & 0x02), 'data_ready': not (st & 0x01),
    }
    alarms = [n for o, b, n in FLAG_BITS if a2[112 + o] & (1 << b)] if ddm['flags_implemented'] else []
    warnings = [n for o, b, n in FLAG_BITS if a2[116 + o] & (1 << b)] if ddm['flags_implemented'] else []
    return {'values': values, 'status': status, 'alarms': alarms, 'warnings': warnings,
            'checksum_ok': checksum_ok(a2, 0, 94, 95)}


def advice(port):
    """Plain-language warnings for the page."""
    out = []
    m = port.get('module')
    if not m:
        return out
    if not m['checksums_ok']:
        out.append('The module memory checksum is wrong. The details below may be unreliable.')
    d = port.get('diagnostics')
    if not m['ddm']['implemented']:
        if m['wavelength_nm']:
            out.append('This module has no diagnostics, so light levels cannot be shown.')
        return out
    if not d:
        return out
    if not d['checksum_ok']:
        out.append('The diagnostics checksum is wrong. The live values may be unreliable.')
    st = d['status']
    if st['tx_fault']:
        out.append('The module reports a transmit fault: its laser is not working.')
    if st['tx_disable'] or st['soft_tx_disable']:
        out.append('The module transmitter is switched off.')
    rx = d['values']['rx']
    if m['wavelength_nm'] and (st['rx_los'] or rx['state'] in ('warning', 'alarm') and rx['value'] < rx['low_warning']):
        hint = ''
        exp = m.get('rx_wavelength')
        if exp and exp.get('nm'):
            hint = ' and that the other end sends at %d nm (this module sends %d nm)' % (exp['nm'], m['wavelength_nm'])
        elif exp and exp.get('choices'):
            hint = ' and that the other end sends at %s nm (this module sends %d nm, check its part number)' % (
                ' or '.join(str(c) for c in exp['choices']), m['wavelength_nm'])
        level = '%s dBm' % rx['dbm'] if rx['dbm'] is not None else 'no light'
        out.append('Receive power is too low (%s, the module warns below %s dBm). Check the fiber, its connectors%s.' % (
            level, rx['low_warning_dbm'], hint))
    tx = d['values']['tx']
    if m['wavelength_nm'] and not st['tx_disable'] and tx['state'] in ('warning', 'alarm') and tx['value'] < tx['low_warning']:
        out.append('Transmit power is low (%s dBm). The laser may be failing.' % tx['dbm'])
    for name in ('temp', 'vcc', 'bias'):
        if d['values'][name]['state'] in ('warning', 'alarm'):
            out.append('%s is outside the module limits.' % {'temp': 'Temperature', 'vcc': 'Supply voltage',
                                                               'bias': 'Laser bias'}[name])
    return out


def port_info(n, names):
    dev = 'dtsec%d' % n
    media, status = link_status(dev)
    port = {
        'device': dev, 'cage': CAGES.get(dev, dev), 'interface': names.get(dev, ''),
        'media': media, 'status': status, 'present': False,
    }
    autoneg = sysctl('dev.dtsec.%d.pcs_autoneg' % n)
    port['pcs_autoneg'] = int(autoneg) if autoneg.isdigit() else None
    phy_modes = sysctl('dev.dtsec.%d.sfp_phy_modes' % n)
    port['phy_modes'] = phy_modes or None

    raw = sysctl('dev.dtsec.%d.sfp_eeprom' % n)
    if len(raw) < 1024:
        return port
    data = bytes.fromhex(raw[:1024])
    a0, a2 = data[:256], data[256:]
    port['present'] = True
    port['raw'] = {'a0': raw[:512], 'a2': raw[512:1024]}
    port['module'] = decode_a0(a0)
    if port['module']['ddm']['implemented']:
        port['diagnostics'] = decode_a2(a2, port['module']['ddm'])
    port['advice'] = advice(port)
    return port


LOG = '/var/log/system/latest.log'
LOG_MATCH = re.compile(r'sfp|SFP|dtsec[34]\b|sff')


def sfp_lines(chunk, since=''):
    today = time.strftime('%Y-%m-%d')
    lines = []
    for line in chunk.splitlines():
        if ' kernel ' not in line or not LOG_MATCH.search(line):
            continue
        m = re.match(r'<\d+>\d+ (\S+) \S+ kernel - - (?:\[meta[^]]*\] )?(.*)', line)
        if not m:
            lines.append({'time': '', 'text': line})
            continue
        stamp = m.group(1)[:19]
        if stamp < since:
            continue
        msg = re.sub(r'^(<\d+>)?(\s*\[\d+\])?\s*', '', m.group(2))
        if 'dpa_add_vlan_if' in msg:
            continue                          # VLAN registrations, not about the module
        shown = stamp[11:19] if stamp[:10] == today else stamp[5:10] + ' ' + stamp[11:19]
        lines.append({'time': shown, 'text': msg})
    return lines


def log_since(offset):
    """Kernel lines about the SFP ports written after offset; a smaller file means it rotated."""
    try:
        with open(LOG, 'rb') as f:
            f.seek(0, 2)
            size = f.tell()
            if offset < 0 or offset > size:
                offset = size if offset < 0 else 0
            f.seek(offset)
            chunk = f.read(min(size - offset, 1 << 20)).decode('utf-8', 'replace')
            end = offset + len(chunk.encode('utf-8'))
    except OSError:
        return {'offset': 0, 'lines': []}
    return {'offset': end, 'lines': sfp_lines(chunk)}


def log_from_boot():
    """Every SFP kernel line since the router started, from all daily log files."""
    m = re.search(r'sec = (\d+)', sysctl('kern.boottime'))
    boot = int(m.group(1)) if m else 0
    since = time.strftime('%Y-%m-%dT%H:%M:%S', time.localtime(boot))
    lines = []
    for path in sorted(glob.glob(os.path.join(os.path.dirname(LOG), 'system_*.log'))):
        day = re.search(r'(\d{8})\.log$', path)
        if day and day.group(1) < since[:10].replace('-', ''):
            continue
        try:
            with open(path, 'rb') as f:
                lines += sfp_lines(f.read().decode('utf-8', 'replace'), since)
        except OSError:
            continue
    try:
        end = os.path.getsize(LOG)
    except OSError:
        end = 0
    return {'offset': end, 'lines': lines[-5000:], 'boot': since.replace('T', ' ')}


def main():
    if len(sys.argv) > 2 and sys.argv[1] == 'log':
        arg = sys.argv[2]
        if arg == 'boot':
            print(json.dumps(log_from_boot()))
        else:
            print(json.dumps(log_since(int(arg) if re.match(r'-?\d+$', arg) else -1)))
        return
    names = interface_names()
    print(json.dumps({'ports': [port_info(n, names) for n in ports()]}))


if __name__ == '__main__':
    sys.exit(main())
