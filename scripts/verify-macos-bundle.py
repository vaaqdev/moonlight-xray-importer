"""Run on macOS to validate the deployed universal Importer and managed Xray."""
import json
import os
from pathlib import Path
import platform
import re
import socket
import subprocess
import sys
import tempfile
import time

app = Path(sys.argv[1]).resolve()
catalina = '--catalina' in sys.argv[2:]
main = app / 'Contents/MacOS/Moonlight'
resources = app / 'Contents/Resources/Xray'
launcher = resources / 'xray-launcher'
for binary in (main, launcher):
    architectures = subprocess.check_output(['lipo', '-archs', str(binary)], text=True)
    assert 'x86_64' in architectures and (catalina or 'arm64' in architectures), architectures
def minimum_os(binary):
    output = subprocess.check_output(['xcrun', 'vtool', '-show-build', str(binary)], text=True)
    versions = re.findall(r'minos\s+(\S+)', output)
    if not versions:
        output = subprocess.check_output(['otool', '-l', str(binary)], text=True)
        versions = re.findall(r'LC_VERSION_MIN_MACOSX\s+cmdsize\s+\d+\s+version\s+(\S+)', output)
    return [tuple(map(int, version.split('.'))) for version in versions]
main_min, launcher_min = minimum_os(main), minimum_os(launcher)
assert main_min and launcher_min and max(launcher_min) <= max(main_min), (main_min, launcher_min)
print('PASS: launcher minimum macOS matches the app:', main_min)
for filename, architecture in ([('xray-x86_64', 'x86_64')] if catalina else [('xray', 'arm64'), ('xray-x86_64', 'x86_64')]):
    assert architecture in subprocess.check_output(['lipo', '-archs', str(resources / filename)], text=True)
assert not list(app.rglob('profile.json'))
subprocess.run(['codesign', '--verify', '--deep', '--strict', str(app)], check=True)
environment = dict(os.environ)
for variable in ('QT_PLUGIN_PATH', 'QT_QPA_PLATFORM_PLUGIN_PATH', 'QML2_IMPORT_PATH', 'QML_IMPORT_PATH',
                 'QTDIR', 'QT_ROOT_DIR', 'DYLD_LIBRARY_PATH', 'DYLD_FRAMEWORK_PATH'):
    environment.pop(variable, None)
version = subprocess.run([str(main), '--version'], capture_output=True, text=True, timeout=30, env=environment)
assert version.returncode == 0 and '6.2.0' in version.stdout + version.stderr, version
print('PASS: signed app launches from its deployed bundle; no bundled profile')
mach_magic = {b'\xcf\xfa\xed\xfe', b'\xfe\xed\xfa\xcf', b'\xca\xfe\xba\xbe', b'\xbe\xba\xfe\xca', b'\xca\xfe\xba\xbf'}
binaries = []
for path in app.rglob('*'):
    if not path.is_file() or path.is_symlink(): continue
    with path.open('rb') as stream:
        if stream.read(4) in mach_magic: binaries.append(path)
for binary in binaries:
    if catalina:
        architectures = subprocess.check_output(['lipo', '-archs', str(binary)], text=True).strip()
        assert architectures == 'x86_64', (binary, architectures)
        versions = minimum_os(binary)
        assert versions and all((version + (0, 0))[:3] <= (10, 15, 0) for version in versions), (binary, versions)
    output = subprocess.check_output(['otool', '-L', str(binary)], text=True)
    for line in output.splitlines():
        if not line.startswith('\t'): continue
        dependency = line.strip().split(' (', 1)[0]
        if dependency.startswith(('/System/', '/usr/lib/')): continue
        if dependency.startswith('@rpath/'):
            resolved = app / 'Contents/Frameworks' / dependency[len('@rpath/'):]
        elif dependency.startswith('@loader_path/'):
            resolved = binary.parent / dependency[len('@loader_path/'):]
        elif dependency.startswith('@executable_path/'):
            resolved = main.parent / dependency[len('@executable_path/'):]
        else:
            raise AssertionError(f'External dependency: {binary}: {dependency}')
        assert resolved.exists(), f'Missing bundled dependency: {binary}: {dependency}'
print(f'PASS: {len(binaries)} Mach-O files audited; all non-system dependencies bundled')
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
