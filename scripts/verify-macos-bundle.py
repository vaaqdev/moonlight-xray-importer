"""Run on macOS to validate the deployed universal Importer and managed Xray."""
import json
import os
from pathlib import Path
import platform
import socket
import subprocess
import sys
import tempfile
import time

app = Path(sys.argv[1]).resolve()
main = app / 'Contents/MacOS/Moonlight'
resources = app / 'Contents/Resources/Xray'
launcher = resources / 'xray-launcher'
for binary in (main, launcher):
    architectures = subprocess.check_output(['lipo', '-archs', str(binary)], text=True)
    assert 'arm64' in architectures and 'x86_64' in architectures, architectures
for filename, architecture in (('xray', 'arm64'), ('xray-x86_64', 'x86_64')):
    assert architecture in subprocess.check_output(['lipo', '-archs', str(resources / filename)], text=True)
assert not list(app.rglob('profile.json'))
subprocess.run(['codesign', '--verify', '--deep', '--strict', str(app)], check=True)
version = subprocess.run([str(main), '--version'], capture_output=True, text=True, timeout=30)
assert version.returncode == 0 and '6.2.0' in version.stdout + version.stderr, version
print('PASS: signed universal app launches from its deployed bundle; no bundled profile')
core = resources / ('xray' if platform.machine() == 'arm64' else 'xray-x86_64')
with tempfile.TemporaryDirectory(prefix='moonlight-macos-test-') as temp:
    config = Path(temp) / 'test.json'
    with socket.socket() as reservation:
        reservation.bind(('127.0.0.1', 0))
        port = reservation.getsockname()[1]
    config.write_text(json.dumps({'inbounds': [{'listen': '127.0.0.1', 'port': port, 'protocol': 'socks',
        'settings': {'auth': 'password', 'accounts': [{'user': 'ci-test', 'pass': 'ci-test'}], 'udp': True}}],
        'outbounds': [{'protocol': 'freedom'}]}))
    subprocess.run([str(core), 'run', '-test', '-c', str(config)], check=True)
    process = subprocess.Popen([str(launcher), str(os.getpid()), str(core), 'run', '-c', str(config)],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    child = None
    try:
        deadline = time.monotonic() + 10
        while True:
            assert process.poll() is None, 'Launcher/core exited before readiness'
            try:
                connection = socket.create_connection(('127.0.0.1', port), timeout=1)
                break
            except OSError:
                if time.monotonic() >= deadline: raise
                time.sleep(.1)
        with connection:
            connection.sendall(bytes.fromhex('050102'))
            assert connection.recv(2) == bytes.fromhex('0502')
            connection.sendall(b'\x01\x07ci-test\x07ci-test')
            assert connection.recv(2) == bytes.fromhex('0100')
        child = int(subprocess.check_output(['pgrep', '-P', str(process.pid)], text=True).strip())
        print('PASS: bundled native Xray and supervisor start an authenticated SOCKS5 tunnel')
    finally:
        process.terminate()
        process.wait(timeout=8)
    assert child
    try:
        os.kill(child, 0)
    except ProcessLookupError:
        pass
    else:
        raise AssertionError('Xray survived supervisor shutdown')
    print('PASS: supervisor shuts down Xray cleanly')
