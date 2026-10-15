#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Instrumented Android-x86 9 boot and native stream integration experiment.

Uses an official, checksum-verified ISO. Only the test initramfs gains a serial
boot-property observer. No accessibility provider or private build recipe lives
here. A pass is not certification of other Android releases or applications.
"""
import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import select
import socket
import subprocess
import time


def run(*args, **kwargs):
    return subprocess.run(args, check=True, **kwargs)


def instrument(iso, root):
    for name in ('kernel', 'initrd.img'):
        run('xorriso', '-osirrox', 'on', '-indev', str(iso),
            '-extract', '/' + name, str(root / name))
    ramdisk = root / 'initrd'
    ramdisk.mkdir()
    run('cpio', '-idm', '--no-absolute-filenames', cwd=ramdisk,
        input=gzip.decompress((root / 'initrd.img').read_bytes()),
        stdout=subprocess.DEVNULL)
    init = ramdisk / 'init'
    original = init.read_text()
    marker = 'exec ${SWITCH:-switch_root} /android /init'
    assert original.count(marker) == 1, 'unexpected upstream initramfs layout'
    observer = r'''
# Test-only serial oracle. Android property service, not a screenshot, proves boot.
(
  n=0
  while [ "$n" -lt 480 ]; do
    if [ "$(/bin/busybox chroot /android /system/bin/getprop sys.boot_completed 2>/dev/null)" = 1 ]; then
      printf '\nMEGAS_ANDROID_READY version='
      /bin/busybox chroot /android /system/bin/getprop ro.build.version.release
      printf 'MEGAS_ANDROID_API='
      /bin/busybox chroot /android /system/bin/getprop ro.build.version.sdk
      break
    fi
    n=$((n+1))
    /bin/busybox sleep 1
  done
) >/dev/ttyS0 2>&1 &
'''
    init.write_text(original.replace(marker, observer + '\n' + marker))
    files = subprocess.check_output(['find', '.', '-print0'], cwd=ramdisk)
    archive = subprocess.check_output(['cpio', '--null', '-o', '-H', 'newc'],
                                      cwd=ramdisk, input=files)
    (root / 'observed-initrd.img').write_bytes(gzip.compress(archive, mtime=0))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--iso', required=True)
    parser.add_argument('--qemu', required=True)
    parser.add_argument('--output', required=True)
    parser.add_argument('--cgroup', required=True)
    args = parser.parse_args()
    iso, qemu = Path(args.iso).resolve(), Path(args.qemu).resolve()
    root = Path(args.output).resolve()
    root.mkdir(parents=True, exist_ok=True)
    with iso.open('rb') as source:
        sha256 = hashlib.file_digest(source, 'sha256').hexdigest()
    assert sha256 == 'f7eb8fc56f29ad5432335dc054183acf086c539f3990f0b6e9ff58bd6df4604e'
    instrument(iso, root)
    with socket.socket() as reservation:
        reservation.bind(('127.0.0.1', 0))
        port = reservation.getsockname()[1]
    serial = root / 'serial.sock'
    accel = 'kvm' if os.access('/dev/kvm', os.R_OK | os.W_OK) else 'tcg'
    command = [str(qemu), '-L', '/usr/share/qemu', '-accel', accel if accel == 'kvm' else
               'tcg,thread=single,tb-size=32', '-cpu', 'max', '-smp', '1',
               '-m', '1536', '-display', f'megas,port={port},fps=30,bitrate=1200',
               '-vga', 'std', '-kernel', str(root / 'kernel'),
               '-initrd', str(root / 'observed-initrd.img'),
               '-append', 'root=/dev/ram0 androidboot.hardware=android_x86_64 '
                          'androidboot.selinux=permissive console=ttyS0 '
                          'nomodeset xforcevesa SRC= DATA= SETUPWIZARD=0',
               '-cdrom', str(iso), '-serial',
               f'unix:{serial},server=on,wait=off', '-monitor', 'none',
               '-nic', 'user,model=e1000', '-no-reboot']
    cgroup = Path(args.cgroup)

    def enter_budget():
        (cgroup / 'cgroup.procs').write_text(str(os.getpid()))

    log = (root / 'qemu.log').open('wb')
    started = time.monotonic()
    process = subprocess.Popen(command, stdout=log, stderr=log,
                               preexec_fn=enter_budget)
    serial_data = bytearray()
    video_data = bytearray()
    connection = None
    video = None
    boot_seconds = None
    maximum_threads = maximum_rss = 0
    try:
        deadline = started + 480
        while time.monotonic() < deadline:
            assert process.poll() is None, 'Android VM exited before boot completed'
            if connection is None and serial.exists():
                connection = socket.socket(socket.AF_UNIX)
                connection.connect(str(serial))
                connection.setblocking(False)
            # Read continuously throughout boot; never build a video frame queue.
            if video is None:
                try:
                    video = socket.create_connection(('127.0.0.1', port), 0.1)
                    video.setblocking(False)
                except OSError:
                    video = None
            sockets = [s for s in (connection, video) if s is not None]
            ready, _, _ = select.select(sockets, [], [], 0.1)
            for sock in ready:
                data = sock.recv(65536)
                if sock is connection:
                    assert data, 'serial oracle disconnected'
                    serial_data.extend(data)
                    if len(serial_data) > 4 * 1024 * 1024:
                        del serial_data[:1024 * 1024]
                elif not data:
                    video.close()
                    video = None
                elif boot_seconds is not None:
                    video_data.extend(data)
                    assert len(video_data) < 32 * 1024 * 1024
            if boot_seconds is None and b'MEGAS_ANDROID_READY version=9' in serial_data:
                boot_seconds = time.monotonic() - started
                deadline = time.monotonic() + 4
            task = Path(f'/proc/{process.pid}')
            maximum_threads = max(maximum_threads, len(list((task / 'task').iterdir())))
            status = (task / 'status').read_text()
            for line in status.splitlines():
                if line.startswith('VmRSS:'):
                    maximum_rss = max(maximum_rss, int(line.split()[1]) * 1024)
        assert boot_seconds is not None, 'Android sys.boot_completed was never observed'
        assert b'MEGAS_ANDROID_API=28' in serial_data, 'unexpected Android release'
        assert maximum_threads <= 48
        stream = root / 'android.ts'
        stream.write_bytes(video_data)
        probe = json.loads(subprocess.check_output([
            'ffprobe', '-v', 'error', '-show_streams', '-count_frames',
            '-of', 'json', str(stream)]))['streams'][0]
        assert probe['codec_name'] == 'h264' and int(probe['nb_read_frames']) >= 3
        run('ffmpeg', '-v', 'error', '-threads', '1', '-i', str(stream),
            '-frames:v', '3', '-threads', '1', '-f', 'null', '-')
        report = {'android': '9.0-r2', 'api': 28, 'accel': accel,
                  'instrumented_test_initrd': True, 'iso_sha256': sha256,
                  'boot_completed_seconds': round(boot_seconds, 3),
                  'maximum_qemu_host_threads': maximum_threads,
                  'maximum_qemu_rss_bytes': maximum_rss,
                  'decoded_stream_frames': int(probe['nb_read_frames']),
                  'cgroup_memory_events': (cgroup / 'memory.events').read_text(),
                  'cgroup_cpu_stat': (cgroup / 'cpu.stat').read_text(),
                  'scope': 'Android 9 boot and software stream only; no agent task benchmark'}
        (root / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps(report, indent=2))
    finally:
        (root / 'serial.log').write_bytes(serial_data)
        for sock in (connection, video):
            if sock is not None:
                sock.close()
        if process.poll() is None:
            process.terminate()
        process.wait(timeout=15)
        log.close()
        print((root / 'qemu.log').read_text(errors='replace'))


if __name__ == '__main__':
    main()
