#!/usr/bin/env python3
"""Real-QEMU contract checks; stdlib only, no guest OS or assembler needed.

Usage: python3 tests/test_live.py build/megas-headless --report /tmp/live.json
Requires a built controller plus qemu-system-x86_64 and qemu-img on PATH.
"""
import argparse
import base64
import contextlib
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import socket
import struct
import subprocess
import tempfile
import time
import zlib

# BIOS text-mode test guest: banner, hidden cursor, then display each keypress.
# This is test input, not a replacement guest OS. BIOS keyboard handling exercises
# the real QEMU RFB input path; VGA text memory is part of each VM snapshot.
BOOT_PREFIX = bytes.fromhex(
    'fa31c08ed88ec08ed0bc007cfbb80300cd10be3f7cac84c07406b40ecd10ebf5'
    'b401b90020cd10b800b88ec0bfcc0726c705300f30e4cd16b40f268905ebf5'
    '4d4547415320494e544547524154494f4e2047554553543a206c697665206b6579'
    '7320616e6420636865636b706f696e742070726f6f6600')


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def wait_for(predicate, message, seconds=10):
    deadline = time.monotonic() + seconds
    last = None
    while time.monotonic() < deadline:
        try:
            last = predicate()
            if last:
                return last
        except (OSError, ValueError):
            pass
        time.sleep(0.05)
    raise AssertionError(f'{message}; last value={last!r}')


def available_ports(count):
    sockets = []
    try:
        for _ in range(count):
            sock = socket.socket()
            sock.bind(('127.0.0.1', 0))
            sockets.append(sock)
        return [sock.getsockname()[1] for sock in sockets]
    finally:
        for sock in sockets:
            sock.close()


_HOST_PIDS = {}

def host_pid(pid, parent=None):
    """A runner may mount host /proc while using an inner PID namespace."""
    if not Path('/proc/self/status').exists():
        return pid
    if not _HOST_PIDS:
        _HOST_PIDS[os.getpid()] = int(os.readlink('/proc/self'))
    if pid in _HOST_PIDS:
        return _HOST_PIDS[pid]
    expected_parent = _HOST_PIDS.get(parent) if parent is not None else None
    for entry in Path('/proc').iterdir():
        if not entry.name.isdecimal():
            continue
        try:
            fields = dict(line.split(':', 1) for line in (entry / 'status').read_text().splitlines() if ':' in line)
            namespace = fields.get('NSpid', fields.get('Pid', '')).split()
            if namespace and int(namespace[-1]) == pid and (expected_parent is None or int(fields['PPid']) == expected_parent):
                _HOST_PIDS[pid] = int(entry.name)
                return int(entry.name)
        except (OSError, ValueError, KeyError):
            continue
    return pid


def rss_kib(pid, parent=None):
    if os.name == 'nt':
        import ctypes
        from ctypes import wintypes

        class ProcessMemoryCountersEx(ctypes.Structure):
            _fields_ = [
                ('cb', wintypes.DWORD),
                ('PageFaultCount', wintypes.DWORD),
                ('PeakWorkingSetSize', ctypes.c_size_t),
                ('WorkingSetSize', ctypes.c_size_t),
                ('QuotaPeakPagedPoolUsage', ctypes.c_size_t),
                ('QuotaPagedPoolUsage', ctypes.c_size_t),
                ('QuotaPeakNonPagedPoolUsage', ctypes.c_size_t),
                ('QuotaNonPagedPoolUsage', ctypes.c_size_t),
                ('PagefileUsage', ctypes.c_size_t),
                ('PeakPagefileUsage', ctypes.c_size_t),
                ('PrivateUsage', ctypes.c_size_t),
            ]

        kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
        psapi = ctypes.WinDLL('psapi', use_last_error=True)
        kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        kernel32.OpenProcess.restype = wintypes.HANDLE
        kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
        kernel32.CloseHandle.restype = wintypes.BOOL
        psapi.GetProcessMemoryInfo.argtypes = [
            wintypes.HANDLE, ctypes.POINTER(ProcessMemoryCountersEx), wintypes.DWORD,
        ]
        psapi.GetProcessMemoryInfo.restype = wintypes.BOOL
        handle = kernel32.OpenProcess(0x0400, False, pid)  # PROCESS_QUERY_INFORMATION
        if not handle:
            return None
        try:
            counters = ProcessMemoryCountersEx()
            counters.cb = ctypes.sizeof(counters)
            if not psapi.GetProcessMemoryInfo(handle, ctypes.byref(counters), counters.cb):
                return None
            return int(counters.WorkingSetSize) // 1024
        finally:
            kernel32.CloseHandle(handle)

    path = Path(f'/proc/{host_pid(pid, parent)}/status')
    if not path.exists():
        return None
    for line in path.read_text().splitlines():
        if line.startswith('VmRSS:'):
            return int(line.split()[1])
    return None


def pid_alive(pid):
    if not pid:
        return False
    if os.name == 'nt':
        import ctypes
        kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel32.OpenProcess.argtypes = [ctypes.c_uint32, ctypes.c_int, ctypes.c_uint32]
        kernel32.OpenProcess.restype = ctypes.c_void_p
        kernel32.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
        kernel32.WaitForSingleObject.restype = ctypes.c_uint32
        kernel32.CloseHandle.argtypes = [ctypes.c_void_p]
        kernel32.CloseHandle.restype = ctypes.c_int
        handle = kernel32.OpenProcess(0x00100000, False, pid)  # SYNCHRONIZE
        if not handle:
            return False
        try:
            return kernel32.WaitForSingleObject(handle, 0) == 0x00000102  # WAIT_TIMEOUT
        finally:
            kernel32.CloseHandle(handle)
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    return True


def png_pixels(path):
    """Validate CRCs, decompress and reconstruct PNG scanlines, hash pixels."""
    data = Path(path).read_bytes()
    require(data[:8] == b'\x89PNG\r\n\x1a\n', 'PNG signature invalid')
    cursor, compressed, metadata = 8, bytearray(), []
    header = None
    seen_end = False
    while cursor + 12 <= len(data):
        length = struct.unpack_from('>I', data, cursor)[0]
        name = data[cursor+4:cursor+8]
        chunk = data[cursor+8:cursor+8+length]
        require(cursor + length + 12 <= len(data), 'PNG truncated chunk')
        crc = struct.unpack_from('>I', data, cursor+8+length)[0]
        require(zlib.crc32(name + chunk) & 0xffffffff == crc, 'PNG CRC invalid')
        if name == b'IHDR':
            header = struct.unpack('>IIBBBBB', chunk)
        elif name == b'IDAT':
            compressed.extend(chunk)
        elif name in (b'tEXt', b'iTXt', b'zTXt'):
            metadata.append(chunk.decode('utf-8', 'replace'))
        elif name == b'IEND':
            seen_end = True
        cursor += length + 12
    require(header and seen_end, 'PNG header/end missing')
    width, height, depth, color, compression, filtering, interlace = header
    require(depth == 8 and color in (2, 6), 'Unexpected PNG pixel layout')
    require((compression, filtering, interlace) == (0, 0, 0), 'Unexpected PNG coding')
    channels = 3 if color == 2 else 4
    stride = width * channels
    raw = zlib.decompress(compressed)
    require(len(raw) == (stride + 1) * height, 'PNG scanline length invalid')
    rows, previous = bytearray(), bytes(stride)
    for y in range(height):
        start = y * (stride + 1)
        mode = raw[start]
        row = bytearray(raw[start+1:start+1+stride])
        for x in range(stride):
            left = row[x-channels] if x >= channels else 0
            up = previous[x]
            upper_left = previous[x-channels] if x >= channels else 0
            if mode == 1:
                row[x] = (row[x] + left) & 255
            elif mode == 2:
                row[x] = (row[x] + up) & 255
            elif mode == 3:
                row[x] = (row[x] + ((left + up) // 2)) & 255
            elif mode == 4:
                p = left + up - upper_left
                distances = (abs(p-left), abs(p-up), abs(p-upper_left))
                predictor = (left, up, upper_left)[distances.index(min(distances))]
                row[x] = (row[x] + predictor) & 255
            else:
                require(mode == 0, f'PNG filter invalid: {mode}')
        rows.extend(row)
        previous = row
    return {'width': width, 'height': height,
            'pixel_sha256': hashlib.sha256(rows).hexdigest(),
            'bytes': len(data), 'metadata': metadata}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', nargs='?', default='build/megas-headless')
    parser.add_argument('--qemu', default='qemu-system-x86_64')
    parser.add_argument('--qemu-img', default='qemu-img')
    parser.add_argument('--report')
    parser.add_argument('--workdir', help='Retain a named test directory for forensic inspection')
    args = parser.parse_args()
    binary = Path(args.binary).resolve()
    qemu = shutil.which(args.qemu)
    qemu_img = shutil.which(args.qemu_img)
    tesseract = shutil.which(os.environ.get('MEGAS_TESSERACT', 'tesseract'))
    require(binary.is_file(), f'Controller binary missing: {binary}')
    require(qemu and qemu_img, 'Install test dependencies qemu-system-x86_64 and qemu-img')
    report = {'environment': {
        'platform': platform.platform(), 'cpu_count': os.cpu_count(),
        'qemu': subprocess.check_output([qemu, '--version'], text=True).splitlines()[0],
        'binary': str(binary), 'guest_ram_mb': 64, 'acceleration': 'TCG',
    }, 'checks': [], 'measurements': {}}
    for name in ('memory.max', 'cpu.max'):
        source = Path('/sys/fs/cgroup') / name
        if source.is_file():
            report['environment'][name] = source.read_text().strip()
    def passed(name, **details):
        report['checks'].append({'name': name, 'passed': True, **details})
        print(f'PASS {name}', flush=True)
    owner, owned_pid = None, None
    start = time.monotonic()
    try:
        if args.workdir:
            Path(args.workdir).mkdir(parents=True, exist_ok=True)
        context = contextlib.nullcontext(args.workdir) if args.workdir else tempfile.TemporaryDirectory(prefix='megas-live-')
        with context as directory:
            runtime = Path(directory)
            raw = runtime / 'guest.raw'
            disk = runtime / 'guest.qcow2'
            with raw.open('wb') as file:
                file.write(BOOT_PREFIX + bytes(510 - len(BOOT_PREFIX)) + b'\x55\xaa')
                file.truncate(4 * 1024 * 1024)
            subprocess.run([qemu_img, 'convert', '-f', 'raw', '-O', 'qcow2', str(raw), str(disk)],
                           check=True, capture_output=True)
            control_port, qmp_port, vnc_port = available_ports(3)
            common = [str(binary), '--runtime', str(runtime), '--port', str(control_port),
                      '--qmp-port', str(qmp_port), '--vnc-port', str(vnc_port)]
            owner_log = (runtime / 'owner.log').open('w')
            owner = subprocess.Popen(common + ['serve'], stdout=owner_log, stderr=owner_log)
            token = runtime / f'session-{control_port}.token'
            wait_for(lambda: token.is_file() and token.stat().st_size > 0,
                     'Owner credential was not created')
            report['measurements']['owner_idle_rss_kib'] = rss_kib(owner.pid, os.getpid())
            def send(request, ok=True):
                result = subprocess.run(common + ['send', json.dumps(request)],
                                        capture_output=True, text=True, timeout=20)
                require(result.stdout.strip(), f'No JSON response: {result.stderr}')
                response = json.loads(result.stdout)
                if ok:
                    require(response.get('ok') is True, f'Command failed: {request}: {response}')
                return response
            def record_ocr_failure_diagnostics(reason):
                # The test guest contains only a fixed public fixture banner and keys.
                # Preserve a small status/log excerpt before TemporaryDirectory cleanup.
                diagnostic = {'reason': str(reason)[:512]}
                try:
                    current_status = send({'action': 'status'})
                    ocr_status = current_status.get('ocr', {})
                    diagnostic['status_excerpt'] = {
                        'status': ocr_status.get('status'),
                        'message': str(ocr_status.get('message') or '')[:512],
                        'generation': ocr_status.get('generation'),
                        'image_generation': current_status.get('image_generation'),
                        'pid': ocr_status.get('pid'),
                        'peak_memory_bytes': ocr_status.get('peak_memory_bytes'),
                        'text_excerpt': str(ocr_status.get('text') or '')[:512],
                    }
                except Exception as error:
                    diagnostic['status_error'] = f'{type(error).__name__}: {str(error)[:512]}'
                log_path = runtime / 'ocr.log'
                try:
                    log_size = log_path.stat().st_size
                    with log_path.open('rb') as log_file:
                        log_file.seek(max(0, log_size - 4096))
                        log_tail = log_file.read(4096)
                    diagnostic['ocr_process_log_tail'] = log_tail.decode('utf-8', 'replace')
                    diagnostic['ocr_process_log_bytes_retained'] = len(log_tail)
                    diagnostic['ocr_process_log_truncated'] = log_size > len(log_tail)
                except OSError as error:
                    diagnostic['ocr_process_log_error'] = f'{type(error).__name__}: {str(error)[:512]}'
                report.setdefault('diagnostics', {})['ocr_failure'] = diagnostic

            def require_ocr(condition, message):
                if not condition:
                    record_ocr_failure_diagnostics(message)
                require(condition, message)

            def raw_request(request):
                with socket.create_connection(('127.0.0.1', control_port), timeout=5) as sock:
                    sock.sendall((json.dumps(request)+'\n').encode())
                    buf = bytearray()
                    while b'\n' not in buf:
                        part = sock.recv(65536)
                        if not part:
                            break
                        buf.extend(part)
                    return json.loads(buf)
            wait_for(lambda: send({'action': 'status'}).get('ok'), 'Owner did not start')
            unauthorized = raw_request({'action': 'start', 'disk': str(disk)})
            require(unauthorized.get('ok') is False, 'Missing token was accepted')
            unauthorized = raw_request({'action': 'start', 'auth': 'wrong', 'disk': str(disk)})
            require(unauthorized.get('ok') is False, 'Wrong token was accepted')
            passed('control_requires_session_token')
            second = subprocess.run(common + ['serve'], capture_output=True, text=True, timeout=5)
            require(second.returncode != 0, 'Second owner was allowed in the same runtime')
            passed('one_owner_per_runtime')
            response = send({'action': 'start', 'disk': str(disk), 'qemu': qemu,
                             'ram_mb': 64, 'width': 800, 'height': 600})
            wait_for(lambda: send({'action': 'status'})['state'].get('frame_generation', 0) > 0,
                     'Live RFB framebuffer was not received', seconds=15)
            response = send({'action': 'status'})
            owned_pid = response['state']['owned_pid']
            require(owned_pid > 0 and pid_alive(owned_pid), 'QEMU child not owned/running')
            require(response['state'].get('connected') is True,
                    'QMP/RFB-connected status was not reported')
            fresh_age = response['state'].get('fresh_state_age_ms')
            require(isinstance(fresh_age, int) and 0 <= fresh_age < 2000,
                    f'Controller status was stale: {fresh_age!r} ms')
            require(response.get('image_generation') == response['state']['frame_generation'],
                    'Latest image does not match the current native RFB generation')
            require((response.get('image_path') or response.get('latest_image')) and response.get('fallback_text'),
                    'Mandatory image/text state missing')
            report['measurements']['fresh_state_age_ms'] = fresh_age
            report['measurements']['initial_frame_generation'] = response['state']['frame_generation']
            report['measurements']['owner_live_rss_kib'] = rss_kib(owner.pid, os.getpid())
            report['measurements']['qemu_live_rss_kib'] = rss_kib(owned_pid, owner.pid)
            passed('managed_start_and_continuous_live_display', pid=owned_pid)
            # Allow the test guest's BIOS to finish booting before fixed-screen assertions.
            time.sleep(1)
            screenshots = {}
            def capture(label):
                path = runtime / (label + '.png')
                send({'action': 'screenshot', 'path': str(path)})
                screenshots[label] = png_pixels(path)
                return screenshots[label]['pixel_sha256']
            baseline = capture('baseline')
            paused = send({'action': 'pause'})
            require(paused['state']['running'] is False, 'QMP-confirmed pause state missing')
            time.sleep(0.25)
            require(capture('paused') == baseline,
                    'Guest framebuffer changed while paused')
            resumed = send({'action': 'resume'})
            require(resumed['state']['running'] is True, 'QMP-confirmed resume state missing')
            send({'action': 'key', 'key': 'p'})
            baseline = wait_for(lambda: (h if (h := capture('resumed')) != baseline else None),
                                'Resumed guest did not consume the live keypress')
            passed('pause_resume_verified_by_guest_execution_and_pixels')
            send({'action': 'key', 'key': 'a'})
            hash_a = wait_for(lambda: (h if (h := capture('past')) != baseline else None),
                              'RFB key input did not change guest VGA output')
            require(screenshots['past']['metadata'], 'Screenshot lacks state text metadata')
            send({'action': 'checkpoint', 'name': 'past'})
            send({'action': 'key', 'key': 'b'})
            hash_b = wait_for(lambda: (h if (h := capture('future')) != hash_a else None),
                              'Second key input did not change guest VGA output')
            send({'action': 'checkpoint', 'name': 'future'})
            passed('real_guest_input_and_png_crc_pixels_metadata')
            messages = [
                {'jsonrpc': '2.0', 'id': 1, 'method': 'initialize',
                 'params': {'protocolVersion': '2025-06-18', 'capabilities': {},
                            'clientInfo': {'name': 'live-test', 'version': '1'}}},
                {'jsonrpc': '2.0', 'id': 2, 'method': 'tools/list'},
                {'jsonrpc': '2.0', 'id': 3, 'method': 'tools/call',
                 'params': {'name': 'megas_vm', 'arguments': {'action': 'status'}}},
                {'jsonrpc': '2.0', 'id': 4, 'method': 'tools/call',
                 'params': {'name': 'megas_vm', 'arguments': {'action': 'resize', 'width': 0, 'height': 0}}},
            ]
            packet = ''.join(json.dumps(message) + '\n' for message in messages)
            ocr_started_at = time.monotonic()
            adapter_ocr_observations = []
            adapter_run_observations = []
            report['measurements']['ocr_adapter'] = adapter_ocr_observations
            report['measurements']['ocr_adapter_runs'] = adapter_run_observations
            for text_only in (False, True):
                adapter_run_started = time.monotonic()
                result = subprocess.run(common + ['agent'] + (['--text-only'] if text_only else ['--vision']),
                                        input=packet, capture_output=True, text=True, timeout=10)
                adapter_run_observations.append({
                    'text_only': text_only,
                    'elapsed_seconds': round(time.monotonic() - adapter_run_started, 3),
                })
                require(result.returncode == 0, f'Adapter exited: {result.stderr}')
                replies = [json.loads(line) for line in result.stdout.splitlines()]
                require(len(replies) == 4, 'Adapter response count invalid')
                require(replies[1]['result']['tools'][0]['name'] == 'megas_vm',
                        'Adapter did not advertise native VM tool')
                require(replies[3]['result']['isError'] is True,
                        'Rejected tool call was not surfaced as an error')
                for tool_reply in replies[2:4]:
                    content = tool_reply['result']['content']
                    state = json.loads(content[0]['text'])
                    require(state.get('fallback_text') and state['state']['running'],
                            'Adapter omitted verified current VM state')
                    require(state.get('image_generation') == state['state']['frame_generation'],
                            'Adapter state does not identify the current image generation')
                    images = [item for item in content if item['type'] == 'image']
                    if text_only:
                        ocr_result = (content[1].get('text', '')
                                      if len(content) >= 2 and content[1].get('type') == 'text'
                                      else '')
                        adapter_ocr_observations.append({
                            'request_id': tool_reply['id'],
                            'source_image_generation': state.get('image_generation'),
                            'source_ocr_status': state.get('ocr', {}).get('status'),
                            'result': ocr_result,
                        })
                        require_ocr(not images,
                                    'Text-only adapter emitted unsupported image content')
                        require_ocr(bool(ocr_result),
                                    'Text-only adapter omitted its OCR result or honest limitation')
                        if tesseract:
                            require_ocr(ocr_result.startswith('Tesseract OCR for live image generation ') or
                                        ocr_result.startswith('OCR is still running or unavailable within the 1.5-second adapter wait;'),
                                        f'Installed Tesseract returned neither OCR nor an honest pending result: {ocr_result!r}')
                        else:
                            require_ocr('OCR' in ocr_result,
                                        'Text-only adapter did not report a real OCR limitation')
                    else:
                        require(len(images) == 1 and images[0]['mimeType'] == 'image/png',
                                'Vision adapter omitted current PNG image from a tool response')
                        image_file = runtime / f"adapter-{tool_reply['id']}.png"
                        image_file.write_bytes(base64.b64decode(images[0]['data'], validate=True))
                        require(png_pixels(image_file)['pixel_sha256'] == hash_b,
                                'Adapter image differs from actual live guest framebuffer')
            if tesseract:
                ocr_deadline = ocr_started_at + 10
                current = send({'action': 'status'})
                ocr = current.get('ocr', {})
                report['measurements']['ocr_owner_status'] = {
                    'status': ocr.get('status'), 'message': ocr.get('message'),
                    'generation': ocr.get('generation'),
                    'image_generation': current.get('image_generation'),
                    'pid': ocr.get('pid'), 'peak_memory_bytes': ocr.get('peak_memory_bytes'),
                }
                while ocr.get('status') == 'running' and time.monotonic() < ocr_deadline:
                    time.sleep(0.1)
                    current = send({'action': 'status'})
                    ocr = current.get('ocr', {})
                    report['measurements']['ocr_owner_status'] = {
                        'status': ocr.get('status'), 'message': ocr.get('message'),
                        'generation': ocr.get('generation'),
                        'image_generation': current.get('image_generation'),
                        'pid': ocr.get('pid'), 'peak_memory_bytes': ocr.get('peak_memory_bytes'),
                    }
                require_ocr(ocr.get('status') == 'complete',
                            f'Tesseract did not complete within its bounded wait: status={ocr.get("status")!r}, '
                            f'message={ocr.get("message")!r}, generation={ocr.get("generation")!r}, '
                            f'image_generation={current.get("image_generation")!r}, '
                            f'peak_memory_bytes={ocr.get("peak_memory_bytes")!r}')
                require_ocr(ocr.get('generation') == current.get('image_generation'),
                            'OCR result does not match the current image generation')
                peak = ocr.get('peak_memory_bytes')
                require_ocr(isinstance(peak, int) and 0 < peak <= 48 * 1024 * 1024,
                            f'Observed OCR peak memory is missing or exceeds 48 MiB: {peak!r}')
                require_ocr(bool(ocr.get('text')), 'Completed OCR result contains no text')
                verify_packet = ''.join(json.dumps(message) + '\n' for message in (
                    {'jsonrpc': '2.0', 'id': 11, 'method': 'initialize',
                     'params': {'protocolVersion': '2025-06-18', 'capabilities': {},
                                'clientInfo': {'name': 'live-test', 'version': '1'}}},
                    {'jsonrpc': '2.0', 'id': 12, 'method': 'tools/call',
                     'params': {'name': 'megas_vm', 'arguments': {'action': 'status'}}},
                ))
                verified = subprocess.run(common + ['agent', '--text-only'], input=verify_packet,
                                           capture_output=True, text=True, timeout=10)
                require_ocr(verified.returncode == 0,
                            f'Verification adapter exited: {verified.stderr}')
                verified_replies = [json.loads(line) for line in verified.stdout.splitlines()]
                require_ocr(len(verified_replies) == 2,
                            'Verification adapter response count invalid')
                verified_content = verified_replies[1]['result']['content']
                require_ocr(verified_content and verified_content[0]['type'] == 'text',
                            'Verification adapter omitted its owner status')
                verified_state = json.loads(verified_content[0]['text'])
                require_ocr(verified_state.get('image_generation') ==
                            verified_state.get('state', {}).get('frame_generation'),
                            'Verification adapter state does not identify the current image generation')
                require_ocr(verified_state.get('ocr', {}).get('status') == 'complete' and
                            verified_state['ocr'].get('generation') == verified_state.get('image_generation'),
                            'Verification adapter owner status does not contain current completed OCR')
                verified_text = (verified_content[1].get('text', '')
                                 if len(verified_content) >= 2 and verified_content[1]['type'] == 'text'
                                 else '')
                ocr_prefix = 'Tesseract OCR for live image generation '
                require_ocr(verified_text.startswith(ocr_prefix),
                            f'Completed Tesseract result was not returned by text-only adapter: '
                            f'{verified_text or verified_content!r}')
                adapter_generation = verified_text[len(ocr_prefix):].split(':', 1)[0]
                require_ocr(adapter_generation.isdecimal() and
                            int(adapter_generation) == verified_state.get('image_generation'),
                            f'Text-only adapter OCR generation does not match its image: '
                            f'{adapter_generation!r} vs {verified_state.get("image_generation")!r}')
                require_ocr(bool(verified_text.split('\n', 1)[1].strip())
                            if '\n' in verified_text else False,
                            'Text-only adapter returned an empty OCR body after completion')
                report['measurements']['ocr_adapter_verified'] = {
                    'image_generation': verified_state.get('image_generation'),
                    'result': verified_text,
                }
                report['measurements']['ocr'] = {
                    'status': ocr['status'], 'generation': ocr['generation'],
                    'peak_memory_bytes': peak, 'text_bytes': len(ocr['text'].encode('utf-8')),
                    'complete_observed_seconds_after_first_adapter_request':
                        round(time.monotonic() - ocr_started_at, 3),
                }
                passed('real_tesseract_ocr_matches_live_image_and_stays_under_48_mib',
                       generation=ocr['generation'], peak_memory_bytes=peak)
            passed('native_stdio_adapter_vision_image_and_text_fallback')
            hook = subprocess.run(common + ['hook', 'PostToolUse'], capture_output=True,
                                  text=True, timeout=10)
            context = json.loads(hook.stdout)['hookSpecificOutput']
            require(context['hookEventName'] == 'PostToolUse', 'Hook event identity missing')
            require(json.loads(context['additionalContext'])['state']['running'],
                    'Hook omitted current verified VM state')
            passed('harness_hook_mandatory_state_context')
            restore_times = []
            for name, expected in (('past', hash_a), ('future', hash_b), ('past', hash_a), ('future', hash_b)):
                before = time.monotonic()
                send({'action': 'restore', 'name': name})
                wait_for(lambda: capture('restored-'+name) == expected,
                         f'Restoring {name} did not restore guest VGA contents', seconds=5)
                elapsed = (time.monotonic() - before) * 1000
                restore_times.append({'checkpoint': name, 'milliseconds': round(elapsed, 2)})
                require(elapsed < 5000, f'Restore exceeded test target: {elapsed:.1f} ms')
            report['measurements']['restore_observed'] = restore_times
            report['measurements']['owner_after_restores_rss_kib'] = rss_kib(owner.pid, os.getpid())
            passed('past_future_restore_verified_by_guest_pixels', restores=restore_times)
            # Unknown or malformed actions must be explicit repairable failures, not guesses.
            for request in ({'action': 'invented_nonexistent_action'}, {'action': 'click', 'x': -1},
                            {'action': 'restore', 'name': 'does-not-exist'},
                            {'action': 'resize', 'width': 0, 'height': 0}):
                result = send(request, ok=False)
                require(result.get('ok') is False, f'Invalid request claimed success: {request}')
                require(result.get('fallback_text'), 'Rejected command omitted mandatory current state')
                require(owner.poll() is None, 'Malformed command crashed owner')
            send({'action': 'status'})
            passed('bad_commands_preserve_live_owner_and_state')
            history = send({'action': 'steps'})
            require(history.get('checkpoints'), 'Checkpoint history missing')
            require(history['statistics']['actions'] >= 10, 'Action statistics missing')
            journal = (runtime / 'actions.jsonl').read_text()
            require(token.read_text() not in journal, 'Session credential leaked into forensic journal')
            entries = [json.loads(line) for line in journal.splitlines()]
            require(any(item['type'] == 'qmp_response' for item in entries),
                    'Actual QEMU results missing from forensic journal')
            require(any(item['type'] == 'snapshot_result' for item in entries),
                    'Confirmed snapshot results missing from forensic journal')
            passed('steps_records_checkpoint_history_and_audited_results')
            send({'action': 'reset'})
            wait_for(lambda: (h if (h := capture('reset')) != hash_b else None),
                     'Reset did not change actual guest framebuffer')
            passed('reset_changes_actual_guest_display')
            # A replacement controller must import the qcow2 snapshot catalog and
            # restore states created by the previous owner before making new work.
            previous_owner_pid = owned_pid
            owner.terminate()
            owner.wait(timeout=10)
            wait_for(lambda: not pid_alive(previous_owner_pid),
                     'Previous owner exit orphaned QEMU before catalog handoff')
            owner_log.close()
            control_port, qmp_port, vnc_port = available_ports(3)
            common = [str(binary), '--runtime', str(runtime), '--port', str(control_port),
                      '--qmp-port', str(qmp_port), '--vnc-port', str(vnc_port)]
            owner_log = (runtime / 'owner-restarted.log').open('w')
            owner = subprocess.Popen(common + ['serve'], stdout=owner_log, stderr=owner_log)
            token = runtime / f'session-{control_port}.token'
            wait_for(lambda: token.is_file() and token.stat().st_size > 0,
                     'Replacement owner credential was not created')
            wait_for(lambda: send({'action': 'status'}).get('ok'),
                     'Replacement owner did not start')
            send({'action': 'start', 'disk': str(disk), 'qemu': qemu,
                  'ram_mb': 64, 'width': 800, 'height': 600})
            wait_for(lambda: send({'action': 'status'})['state'].get('frame_generation', 0) > 0,
                     'Replacement owner did not receive the live framebuffer', seconds=15)
            restarted_status = send({'action': 'status'})
            owned_pid = restarted_status['state']['owned_pid']
            require(owned_pid > 0 and pid_alive(owned_pid),
                    'Replacement owner did not manage a live QEMU child')
            require(restarted_status['state'].get('checkpoint_catalog_available') is True,
                    'Replacement owner did not verify the existing QEMU checkpoint catalog')
            imported = send({'action': 'steps'})
            imported_checkpoints = imported.get('checkpoints', [])
            imported_by_name = {entry.get('name'): entry for entry in imported_checkpoints}
            for name in ('past', 'future'):
                entry = imported_by_name.get(name)
                require(entry and entry.get('owned') is True and entry.get('restorable') is True,
                        f'Replacement owner did not import restorable managed checkpoint {name!r}')
            imported_names = [entry.get('name') for entry in imported_checkpoints]
            require(imported_names.index('past') < imported_names.index('future'),
                    'QEMU catalog order did not preserve past-before-future save order')
            automatic_pairs = set()
            for name in imported_names:
                if not name or not name.startswith('step-'):
                    continue
                step, separator, edge = name[5:].partition('-')
                if separator and step.isdecimal() and edge in ('before', 'after'):
                    automatic_pairs.add((int(step), edge))
            paired_steps = sorted({step for step, edge in automatic_pairs
                                   if (step, 'before') in automatic_pairs and
                                   (step, 'after') in automatic_pairs})
            require(paired_steps,
                    'Replacement owner did not import any complete automatic checkpoint pair')
            collision_step = max(paired_steps)
            current_actions = imported['statistics']['actions']
            require(collision_step > current_actions,
                    f'No imported automatic checkpoint label is ahead of the replacement action counter: '
                    f'{collision_step} <= {current_actions}')
            while current_actions + 1 < collision_step:
                current_actions = send({'action': 'status'})['statistics']['actions']
            require(current_actions + 1 == collision_step,
                    'Could not align the post-restart native action with an imported logical checkpoint name')
            send({'action': 'pause'})
            after_reowner_action = send({'action': 'steps'})
            after_names = {entry.get('name') for entry in after_reowner_action.get('checkpoints', [])}
            require(f'step-{collision_step}-before-2' in after_names and
                    f'step-{collision_step}-after-2' in after_names,
                    'Post-restart automatic action overwrote or failed to suffix its imported checkpoint labels')
            cross_owner_restores = []
            for name, expected in (('past', hash_a), ('future', hash_b)):
                send({'action': 'restore', 'name': name})
                actual = wait_for(lambda: (h if (h := capture('cross-owner-restored-' + name)) == expected else None),
                                  f'Replacement owner did not restore the previous owner\'s {name} pixels',
                                  seconds=5)
                cross_owner_restores.append({'checkpoint': name, 'pixel_sha256': actual})
            report['measurements']['replacement_owner_after_restores_rss_kib'] = rss_kib(owner.pid, os.getpid())
            report['measurements']['checkpoint_reowner'] = {
                'catalog_available': restarted_status['state'].get('checkpoint_catalog_available'),
                'imported_managed_checkpoints': sum(1 for item in imported_checkpoints
                                                    if item.get('owned') is True),
                'past_precedes_future_in_qmp_catalog': True,
                'automatic_collision_step': collision_step,
                'new_before_label': f'step-{collision_step}-before-2',
                'new_after_label': f'step-{collision_step}-after-2',
                'cross_owner_pixel_restores': cross_owner_restores,
            }
            passed('checkpoint_catalog_reimport_and_unique_automatic_action_after_owner_restart',
                   collision_step=collision_step)
            passed('past_future_pixels_restored_after_owner_restart', restores=cross_owner_restores)
            # Screenshot output and controller RSS must stay bounded, with no frame queue.
            report['measurements']['png'] = screenshots['future']
            for key in ('owner_idle_rss_kib', 'owner_live_rss_kib', 'owner_after_restores_rss_kib',
                        'replacement_owner_after_restores_rss_kib'):
                value = report['measurements'][key]
                if os.name == 'nt':
                    require(value is not None,
                            f'Native Windows process-memory sampling failed: {key}')
                if value is not None:
                    require(value < 100 * 1024, f'Owner exceeds 100 MiB observed: {key}={value} KiB')
            passed('controller_observed_rss_under_100_mib')
            send({'action': 'stop'})
            wait_for(lambda: not pid_alive(owned_pid), 'Managed stop orphaned QEMU')
            passed('managed_stop_reaps_qemu_child')
            # Relaunch then kill the owner gracefully: cleanup must also reap this child.
            send({'action': 'start', 'disk': str(disk), 'qemu': qemu, 'ram_mb': 64})
            owned_pid = send({'action': 'status'})['state']['owned_pid']
            owner.terminate()
            owner.wait(timeout=10)
            wait_for(lambda: not pid_alive(owned_pid), 'Owner exit orphaned QEMU')
            passed('owner_exit_reaps_qemu_child')
            owner_log.close()
        report['passed'] = True
    except Exception as error:
        report['passed'] = False
        report['error'] = f'{type(error).__name__}: {error}'
        print(report['error'], flush=True)
        raise
    finally:
        if owner and owner.poll() is None:
            owner.terminate()
            try:
                owner.wait(timeout=10)
            except subprocess.TimeoutExpired:
                owner.kill()
                owner.wait(timeout=5)
        report['measurements']['elapsed_seconds'] = round(time.monotonic() - start, 2)
        if args.report:
            Path(args.report).write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps(report, indent=2), flush=True)


if __name__ == '__main__':
    main()
