#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Observe actual X11 paint damage and IPC responsiveness; no screenshots."""
import ctypes as c
import json
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import time


def main():
    binary = str(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix='megas-gui-idle-') as directory:
        with socket.socket() as reservation:
            reservation.bind(('127.0.0.1', 0))
            port = str(reservation.getsockname()[1])
        process = subprocess.Popen([binary, '--runtime', directory, '--port', port, 'gui'])
        display = None
        try:
            window = subprocess.check_output([
                'xdotool', 'search', '--sync', '--onlyvisible', '--name',
                '^MegasXLR VM Komuta Merkezi$'], timeout=10).splitlines()[0]
            x11, damage = c.CDLL('libX11.so.6'), c.CDLL('libXdamage.so.1')
            x11.XOpenDisplay.restype = c.c_void_p
            display = x11.XOpenDisplay(None)
            assert display
            for name in ('XPending', 'XFlush', 'XCloseDisplay'):
                getattr(x11, name).argtypes = [c.c_void_p]
            x11.XNextEvent.argtypes = [c.c_void_p, c.c_void_p]
            damage.XDamageQueryExtension.argtypes = [c.c_void_p, c.c_void_p, c.c_void_p]
            damage.XDamageCreate.argtypes = [c.c_void_p, c.c_ulong, c.c_int]
            damage.XDamageCreate.restype = c.c_ulong
            damage.XDamageSubtract.argtypes = [c.c_void_p, c.c_ulong, c.c_ulong, c.c_ulong]
            event_base, error_base = c.c_int(), c.c_int()
            assert damage.XDamageQueryExtension(display, c.byref(event_base), c.byref(error_base))
            handle = damage.XDamageCreate(display, int(window), 3)  # NonEmpty
            x11.XFlush(display)
            # Confirm resize events still wake and redraw the application.
            subprocess.run(['xdotool', 'windowsize', window, '1000', '720'], check=True)
            time.sleep(1)
            event = (c.c_long * 24)()

            def updates():
                count = 0
                while x11.XPending(display):
                    x11.XNextEvent(display, c.byref(event))
                    if c.c_int.from_buffer(event).value == event_base.value:
                        count += 1
                damage.XDamageSubtract(display, handle, 0, 0)
                x11.XFlush(display)
                return count

            updates()
            start = time.monotonic()
            paints, requests, maximum_response = 0, 0, 0
            while time.monotonic() - start < 3:
                paints += updates()
                request_start = time.monotonic()
                result = subprocess.run([binary, '--runtime', directory, '--port', port,
                                         'send', '{"action":"status","include_image":false}'],
                                        stdout=subprocess.PIPE, check=True, timeout=2)
                assert json.loads(result.stdout)['ok']
                maximum_response = max(maximum_response, time.monotonic() - request_start)
                requests += 1
                time.sleep(0.02)
            rate = paints / (time.monotonic() - start)
            assert rate < 20, f'idle GUI still repaints continuously: {rate:.1f} damage events/s'
            assert maximum_response < 1, 'idle redraw throttled agent control'
            print(json.dumps({'idle_damage_events_per_second': round(rate, 2),
                              'control_requests': requests,
                              'maximum_status_response_ms': round(maximum_response * 1000, 2),
                              'scope': 'native GUI idle paint and IPC, not Android performance'}))
        finally:
            if display:
                x11.XCloseDisplay(display)
            process.send_signal(signal.SIGINT)
            process.wait(timeout=15)
            assert process.returncode == 0


if __name__ == '__main__':
    main()
