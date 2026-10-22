#!/usr/bin/env python3
"""Exercise the real stdio adapter against a visible native manager."""
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import time


def port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


def process_alive(pid):
    if os.name == 'nt':
        result = subprocess.run(['tasklist', '/FI', f'PID eq {pid}'],
                                capture_output=True, text=True)
        return str(pid) in result.stdout
    try:
        os.kill(pid, 0)
    except OSError:
        return False
    return True


def stop_process(pid):
    if os.name == 'nt':
        subprocess.run(['taskkill', '/PID', str(pid), '/F'], capture_output=True)
        return
    try:
        os.kill(pid, signal.SIGTERM)
    except OSError:
        pass


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

    # A harness launches only `megas agent`, so the adapter owns the controller
    # it speaks to: a call made while no owner runs starts one and answers from
    # it. The started owner is detached, so it outlives the adapter and the next
    # adapter reuses it instead of starting a second one.
    with tempfile.TemporaryDirectory(prefix='megas-agent-owner-') as runtime:
        control = port()
        home = Path(runtime) / 'home'
        home.mkdir(exist_ok=True)
        env = dict(os.environ)
        env['HOME'] = str(home)
        env['MEGAS_OWNER_MODE'] = 'serve'
        common = [binary, '--runtime', runtime, '--port', str(control)]
        lock = Path(runtime) / f'owner-{control}.lock'
        assert not lock.exists(), 'test runtime already has an owner'
        bridge = subprocess.Popen(
            common + ['agent', '--text-only'], env=env,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True)
        owner_pid = None
        try:
            def call(request):
                bridge.stdin.write(json.dumps(request) + '\n')
                bridge.stdin.flush()
                result = bridge.stdout.readline()
                assert result, bridge.stderr.read()
                return json.loads(result)

            call({'jsonrpc': '2.0', 'id': 1, 'method': 'initialize',
                  'params': {'protocolVersion': '2025-06-18',
                             'capabilities': {},
                             'clientInfo': {'name': 'test', 'version': '1'}}})
            status = call({'jsonrpc': '2.0', 'id': 2, 'method': 'tools/call',
                           'params': {'name': 'megas_vm',
                                      'arguments': {'action': 'status'}}})
            assert status['result']['isError'] is False, status
            state = json.loads(status['result']['content'][0]['text'])
            assert state.get('ok') is True, state
            assert 'phase' in state.get('state', {}), state
            assert lock.exists(), 'adapter did not record the owner it started'
            owner_pid = int(lock.read_text().strip())
            assert owner_pid > 0, owner_pid
            bridge.stdin.close()
            assert bridge.wait(timeout=5) == 0
            bridge = None
            assert process_alive(owner_pid), 'started owner did not outlive the adapter'
            reused = subprocess.run(common + ['status'], env=env,
                                    capture_output=True, text=True, timeout=10)
            assert reused.returncode == 0 and '"ok":true' in reused.stdout, reused.stdout
            print('PASS: adapter starts the owner it needs, the owner outlives it, '
                  'and a later client answers from the same session')
        finally:
            if bridge is not None:
                bridge.kill()
                bridge.wait(timeout=5)
            if owner_pid:
                stop_process(owner_pid)
                for _ in range(50):
                    if not process_alive(owner_pid):
                        break
                    time.sleep(.1)


if __name__ == '__main__':
    main()
