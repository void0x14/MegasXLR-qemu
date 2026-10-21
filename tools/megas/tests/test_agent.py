#!/usr/bin/env python3
"""Exercise the real stdio adapter against a visible native manager."""
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time


def port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


def main():
    binary = str(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix='megas-agent-test-') as runtime:
        control = port()
        # The named VM registry lives under HOME, so the test never writes
        # into the home directory of the person running it.
        home = Path(runtime) / 'home'
        home.mkdir(exist_ok=True)
        env = dict(os.environ)
        env['HOME'] = str(home)
        env['XDG_CONFIG_HOME'] = str(home / '.config')
        common = [binary, '--runtime', runtime, '--port', str(control)]
        manager = subprocess.Popen(
            common + ['serve'], env=env,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        bridge = None
        try:
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                if manager.poll() is not None:
                    raise AssertionError(manager.stdout.read())
                try:
                    with socket.create_connection(('127.0.0.1', control), .1):
                        break
                except OSError:
                    time.sleep(.02)
            else:
                raise AssertionError('native manager did not open control port')
            bridge = subprocess.Popen(
                common + ['agent', '--text-only'],
                env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, text=True)

            def call(request):
                bridge.stdin.write(json.dumps(request) + '\n')
                bridge.stdin.flush()
                result = bridge.stdout.readline()
                assert result, bridge.stderr.read()
                return json.loads(result)

            init = call({'jsonrpc': '2.0', 'id': 1, 'method': 'initialize',
                         'params': {'protocolVersion': '2025-06-18',
                                    'capabilities': {},
                                    'clientInfo': {'name': 'test', 'version': '1'}}})
            assert init['result']['capabilities']['tools'] == {}
            tools = call({'jsonrpc': '2.0', 'id': 'tools', 'method': 'tools/list'})
            assert tools['id'] == 'tools'
            assert tools['result']['tools'][0]['name'] == 'megas_vm'
            status = call({'jsonrpc': '2.0', 'id': 'state-identity',
                           'method': 'tools/call',
                           'params': {'name': 'megas_vm',
                                      'arguments': {'action': 'status'}}})
            assert status['id'] == 'state-identity'
            items = status['result']['content']
            state = json.loads(items[0]['text'])
            assert 'state' in state or 'phase' in state
            assert all(item['type'] == 'text' for item in items)
            assert any('not been OCR' in item['text'] for item in items)
            bad = call({'jsonrpc': '2.0', 'id': 4, 'method': 'tools/call',
                        'params': {'name': 'megas_vm',
                                   'arguments': {'action': 'imaginary-dangerous-action'}}})
            assert bad['result']['isError'] is True
            assert 'repair' in bad['result']['content'][0]['text']
            malformed = call({'jsonrpc': '2.0', 'id': 5, 'method': 'tools/call',
                              'params': {'name': 'unknown', 'arguments': {}}})
            assert malformed['error']['code'] == -32602
            bridge.stdin.write('{"jsonrpc":\n')
            bridge.stdin.flush()
            assert json.loads(bridge.stdout.readline())['error']['code'] == -32700
            recovered = call({'jsonrpc': '2.0', 'id': 6, 'method': 'ping'})
            assert recovered['id'] == 6 and recovered['result'] == {}
            bridge.stdin.close()
            assert bridge.wait(timeout=5) == 0
            bridge = None
            print('PASS: stdio lifecycle, identifiers, tool schema, mandatory state, '
                  'text-only fallback, repair result, invalid JSON recovery')
        finally:
            if bridge is not None:
                bridge.kill()
                bridge.wait(timeout=5)
            manager.terminate()
            try:
                manager.wait(timeout=5)
            except subprocess.TimeoutExpired:
                manager.kill()
                manager.wait(timeout=5)


if __name__ == '__main__':
    main()
