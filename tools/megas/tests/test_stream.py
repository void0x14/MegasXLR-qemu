#!/usr/bin/env python3
"""Real sockets and encoded MPEG-TS passthrough; synthetic source, not Android."""
import json
import shutil
import socket
import subprocess
import sys
import time
import unittest
from pathlib import Path

FIXTURE = str(Path(sys.argv.pop(1)).resolve())
TOKEN = 'a' * 64


def listener():
    sock = socket.socket()
    sock.bind(('127.0.0.1', 0))
    sock.listen()
    sock.settimeout(4)
    return sock


class StreamTests(unittest.TestCase):
    def setUp(self):
        self.control_server, self.video_server, reservation = listener(), listener(), listener()
        self.port = reservation.getsockname()[1]
        reservation.close()
        self.process = subprocess.Popen([
            FIXTURE, str(self.control_server.getsockname()[1]),
            str(self.video_server.getsockname()[1]), str(self.port), TOKEN,
        ], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.control = self.control_server.accept()[0]
        self.video = self.video_server.accept()[0]
        self.video.settimeout(4)
        self.clients = []

    def tearDown(self):
        for sock in self.clients + [self.control, self.video, self.control_server, self.video_server]:
            sock.close()
        self.process.terminate()
        try:
            out, err = self.process.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.kill()
            out, err = self.process.communicate(timeout=5)
        self.assertEqual(self.process.returncode, 0, err.decode(errors='replace'))
        self.metrics = json.loads(out)

    def request(self, token=TOKEN):
        client = socket.create_connection(('127.0.0.1', self.port), 2)
        client.settimeout(3)
        self.clients.append(client)
        client.sendall(('GET /live.ts HTTP/1.1\r\nHost: localhost\r\n'
                        f'Authorization: Bearer {token}\r\n\r\n').encode())
        data = b''
        while b'\r\n\r\n' not in data:
            part = client.recv(4096)
            self.assertTrue(part)
            data += part
        header, body = data.split(b'\r\n\r\n', 1)
        return client, header, body

    def ready(self):
        self.video.sendall(b'\x47\x1f\xff\x10' + b'\xff' * 184)
        time.sleep(.02)

    def test_auth_and_unavailable_are_explicit(self):
        _, header, _ = self.request('b' * 64)
        self.assertIn(b'403 Forbidden', header)
        _, header, _ = self.request()
        self.assertIn(b'503 Service Unavailable', header)

    def test_fragmented_stream_is_byte_exact_and_disconnect_closes_viewers(self):
        self.ready()
        client, header, data = self.request()
        self.assertIn(b'200 OK', header)
        self.assertIn(b'video/mp2t', header)
        self.assertNotIn(TOKEN.encode(), header)
        self.assertEqual(data, b'')
        expected = b''.join(b'\x47\x1f\xff\x10' + bytes([i]) * 184 for i in range(64))
        for begin in range(0, len(expected), 97):
            self.video.sendall(expected[begin:begin+97])
        while len(data) < len(expected):
            data += client.recv(len(expected) - len(data))
        self.assertEqual(data, expected)
        self.video.shutdown(socket.SHUT_WR)
        self.assertEqual(client.recv(1), b'')

    def test_malformed_transport_closes_without_png_fallback(self):
        self.ready()
        client, _, _ = self.request()
        self.video.sendall(b'X' * 188)
        self.assertEqual(client.recv(1), b'')

    @unittest.skipUnless(shutil.which('ffmpeg'), 'ffmpeg is a test-only dependency')
    def test_real_h264_stream_remains_decodable_without_reencoding(self):
        encoded = subprocess.run([
            'ffmpeg', '-v', 'error', '-f', 'lavfi', '-i', 'testsrc2=size=160x96:rate=10',
            '-frames:v', '10', '-c:v', 'libx264', '-threads', '1', '-preset', 'ultrafast',
            '-tune', 'zerolatency', '-g', '5', '-f', 'mpegts', 'pipe:1',
        ], check=True, capture_output=True).stdout
        self.ready()
        client, _, data = self.request()
        for begin in range(0, len(encoded), 188 * 16):
            self.video.sendall(encoded[begin:begin+188*16])
            time.sleep(.002)
        while len(data) < len(encoded):
            data += client.recv(len(encoded) - len(data))
        self.assertEqual(data, encoded)
        decoded = subprocess.run([
            'ffmpeg', '-v', 'error', '-f', 'mpegts', '-i', 'pipe:0',
            '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-threads', '1', 'pipe:1',
        ], input=data, capture_output=True, check=True).stdout
        self.assertEqual(len(decoded), 10 * 160 * 96 * 3)


if __name__ == '__main__':
    unittest.main()
