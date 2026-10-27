#!/usr/local/bin/python3

"""
Streams CPU use for the Mono Gateway CPU widget: each core, and the whole
CPU as user, system, interrupt and total, in percent.
"""

import argparse
import ctypes
import json
import os
import sys
import time

# By name: ctypes.util.find_library() would run the C compiler to find it.
_libc = ctypes.CDLL('libc.so.7', use_errno=True)


def cp_times():
    # Five counters per core: user, nice, system, interrupt, idle. Read in
    # this process: starting sysctl(8) every second costs more than the rest.
    size = ctypes.c_size_t(0)
    _libc.sysctlbyname(b'kern.cp_times', None, ctypes.byref(size), None, 0)
    values = (ctypes.c_long * (size.value // ctypes.sizeof(ctypes.c_long)))()
    if _libc.sysctlbyname(b'kern.cp_times', values, ctypes.byref(size), None, 0) != 0:
        raise OSError(ctypes.get_errno(), 'kern.cp_times')
    return [list(values[i:i + 5]) for i in range(0, len(values), 5)]


def percent(part, whole):
    return round(100.0 * part / whole, 1) if whole > 0 else 0.0


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--interval', help='poll interval', default='1')
    interval = max(float(parser.parse_args().interval), 0.5)

    last = cp_times()
    while True:
        time.sleep(interval)
        now = cp_times()
        cores = []
        total = [0] * 5
        for old, new in zip(last, now):
            delta = [n - o for n, o in zip(new, old)]
            cores.append(percent(sum(delta) - delta[4], sum(delta)))
            total = [t + d for t, d in zip(total, delta)]
        last = now
        whole = sum(total)
        result = {
            'cores': cores,
            'total': percent(whole - total[4], whole),
            'user': percent(total[0] + total[1], whole),
            'sys': percent(total[2], whole),
            'intr': percent(total[3], whole)
        }
        try:
            print(f"event: message\ndata: {json.dumps(result)}\n\n", flush=True)
        except BrokenPipeError:
            # The page went away; stop without a second error on exit.
            os.dup2(os.open(os.devnull, os.O_WRONLY), sys.stdout.fileno())
            sys.exit(0)
