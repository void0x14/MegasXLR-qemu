#!/usr/bin/env python3
"""Decode the real native encoder and QEMU display using independent FFmpeg.

The BIOS fixture is deliberately not an Android compatibility test.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import socket
import subprocess
import time

from test_live import BOOT_PREFIX, available_ports, wait_for


def decode(path):
    return subprocess.run([
        'ffmpeg', '-v', 'error', '-threads', '1', '-i', str(path),
        '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-threads', '1', '-'],
        check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE).stdout


def fixture_test(executable, root):
    path = root / 'colors.ts'
    with path.open('wb') as output:
        subprocess.run([executable], stdout=output, check=True)
    data = path.read_bytes()
    assert len(data) % 188 == 0
    assert all(data[i] == 0x47 for i in range(0, len(data), 188))
    probe = json.loads(subprocess.check_output([
        'ffprobe', '-v', 'error', '-count_frames', '-show_streams',
        '-show_packets', '-of', 'json', str(path)]))
    stream = probe['streams'][0]
    assert stream['codec_name'] == 'h264' and stream['pix_fmt'] == 'yuv420p'
    assert stream['width'] == 128 and stream['height'] == 96
    assert int(stream['nb_read_frames']) == 60
    stamps = [int(p['pts']) for p in probe['packets']]
    assert all(b - a == 3000 for a, b in zip(stamps, stamps[1:])), stamps
    raw = decode(path)
    frame_size = 128 * 96 * 3
    assert len(raw) == frame_size * 60
    for frame, channel in ((10, 0), (30, 1), (50, 2)):
        rgb = raw[frame * frame_size + 300:frame * frame_size + 303]
        assert rgb[channel] > 240 and sum(rgb) < 275, (frame, list(rgb))
    return {'frames': 60, 'codec': 'h264', 'timestamp_wrap': 'passed',
            'color_order': 'passed', 'bytes': len(data)}


def receive(sock, seconds):
    data = bytearray()
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        try:
            chunk = sock.recv(65536)
        except TimeoutError:
            continue
        if not chunk:
            raise AssertionError('native stream disconnected unexpectedly')
        data.extend(chunk)
        assert len(data) < 16 * 1024 * 1024, 'unexpected bitrate growth'
    return data


def native_test(qemu, root):
    port = available_ports(1)[0]
    disk = root / 'bios.raw'
    disk.write_bytes(BOOT_PREFIX.ljust(510, b'\0') + b'\x55\xaa')
    qmp_path = root / 'qmp.sock'
    log = (root / 'qemu.log').open('wb')
    process = subprocess.Popen([
        qemu, '-accel', 'tcg,thread=single,tb-size=16', '-m', '64', '-smp', '1',
        '-display', f'megas,port={port},fps=30,bitrate=700',
        '-vga', 'std', '-drive', f'file={disk},format=raw,if=floppy',
        '-boot', 'a', '-qmp', f'unix:{qmp_path},server=on,wait=off',
        '-nic', 'none', '-serial', 'none', '-monitor', 'none',
    ], stdout=log, stderr=log)
    try:
        wait_for(lambda: qmp_path.exists(), 'fork QEMU did not start')
        qmp = socket.socket(socket.AF_UNIX)
        qmp.connect(str(qmp_path))
        qmp.settimeout(5)
        command = qmp.makefile('rwb', buffering=0)
        assert 'QMP' in json.loads(command.readline())

        def execute(name, arguments=None):
            request = {'execute': name, 'id': name}
            if arguments is not None:
                request['arguments'] = arguments
            command.write(json.dumps(request).encode() + b'\n')
            while True:
                response = json.loads(command.readline())
                if response.get('id') == name:
                    assert 'return' in response, response
                    return response['return']

        execute('qmp_capabilities')
        options = execute('query-display-options')
        assert options['type'] == 'megas' and options['port'] == port
        # Allow the BIOS to enter the fixture before capturing the stable screen.
        time.sleep(2)
        with socket.create_connection(('127.0.0.1', port), timeout=5) as video:
            video.settimeout(0.2)
            before = receive(video, 2)
            threads = len(list(Path(f'/proc/{process.pid}/task').iterdir()))
            assert threads <= 12, f'unexpected host thread count {threads}'
            execute('send-key', {'keys': [{'type': 'qcode', 'data': 'x'}]})
            after = receive(video, 2)
        # Both samples must decode independently after their next IDR.
        before_path, after_path = root / 'before.ts', root / 'after.ts'
        before_path.write_bytes(before)
        after_path.write_bytes(after)
        probe = json.loads(subprocess.check_output([
            'ffprobe', '-v', 'error', '-show_streams', '-of', 'json',
            str(before_path)]))['streams'][0]
        size = probe['width'] * probe['height'] * 3
        before_raw, after_raw = decode(before_path), decode(after_path)
        assert len(before_raw) >= 5 * size and len(after_raw) >= 5 * size
        first = hashlib.sha256(before_raw[-size:]).hexdigest()
        last = hashlib.sha256(after_raw[-size:]).hexdigest()
        assert first != last, 'native video did not reflect actual guest input'
        # Disconnect/reconnect obtains a fresh, independently decodable IDR.
        with socket.create_connection(('127.0.0.1', port), timeout=5) as video:
            video.settimeout(0.2)
            reconnect = receive(video, 2)
        path = root / 'reconnected.ts'
        path.write_bytes(reconnect)
        assert len(decode(path)) >= 5 * size
        execute('quit')
        process.wait(timeout=10)
        assert process.returncode == 0
        return {'guest': 'BIOS keyboard fixture, not Android',
                'resolution': [probe['width'], probe['height']],
                'qemu_host_threads': threads, 'input_changes_video': True,
                'reconnect_decode': True, 'qemu_exit': process.returncode,
                'before_sha256': first, 'after_sha256': last}
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=10)
        log.close()
        print((root / 'qemu.log').read_text(errors='replace'))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--encoder', required=True)
    parser.add_argument('--qemu', required=True)
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    root = Path(args.output).resolve()
    root.mkdir(parents=True, exist_ok=True)
    report = {'encoder': fixture_test(os.path.abspath(args.encoder), root)}
    report['qemu'] = native_test(os.path.abspath(args.qemu), root)
    (root / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
