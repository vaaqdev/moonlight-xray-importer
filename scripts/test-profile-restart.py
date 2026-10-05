"""Windows UI regression: first import, Later, automatic relaunch and invalid JSON."""
import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import subprocess
import re
import socket
import sys
import tempfile
import time
import zipfile

kernel = ctypes.WinDLL('kernel32', use_last_error=True)
psapi = ctypes.WinDLL('psapi', use_last_error=True)
kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
kernel.OpenProcess.restype = wintypes.HANDLE
kernel.CloseHandle.argtypes = [wintypes.HANDLE]
kernel.TerminateProcess.argtypes = [wintypes.HANDLE, wintypes.UINT]
kernel.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD)]
psapi.EnumProcesses.argtypes = [ctypes.POINTER(wintypes.DWORD), wintypes.DWORD, ctypes.POINTER(wintypes.DWORD)]
user = ctypes.WinDLL('user32', use_last_error=True)
CALLBACK = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
user.EnumWindows.argtypes = [CALLBACK, wintypes.LPARAM]
user.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
user.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]

def matching_processes(exe):
    pids = (wintypes.DWORD * 8192)()
    size = wintypes.DWORD()
    assert psapi.EnumProcesses(pids, ctypes.sizeof(pids), ctypes.byref(size))
    result = []
    for pid in pids[:size.value // 4]:
        handle = kernel.OpenProcess(0x1000, False, pid)
        if not handle: continue
        try:
            text = ctypes.create_unicode_buffer(32768)
            length = wintypes.DWORD(len(text))
            if kernel.QueryFullProcessImageNameW(handle, 0, text, ctypes.byref(length)) and Path(text.value) == exe:
                result.append(pid)
        finally:
            kernel.CloseHandle(handle)
    return result

def close_gui(pid):
    @CALLBACK
    def callback(hwnd, _):
        current = wintypes.DWORD()
        user.GetWindowThreadProcessId(hwnd, ctypes.byref(current))
        if current.value == pid: user.PostMessageW(hwnd, 0x10, 0, 0)
        return True
    user.EnumWindows(callback, 0)

def ui(pid, profile, action):
    powershell = Path(os.environ['SystemRoot']) / 'System32/WindowsPowerShell/v1.0/powershell.exe'
    result = subprocess.run([str(powershell), '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
        str(Path(__file__).with_name('test-profile-restart-ui.ps1')), '-AppPid', str(pid),
        '-ProfilePath', str(profile), '-Action', action], capture_output=True, text=True, timeout=45)
    print(result.stdout, flush=True)
    assert result.returncode == 0, result.stderr

def verify_tunnel(exe):
    pids = matching_processes(exe)
    assert len(pids) == 1, ('Expected exactly one bundled Xray', pids)
    powershell = Path(os.environ['SystemRoot']) / 'System32/WindowsPowerShell/v1.0/powershell.exe'
    command = f'(Get-CimInstance Win32_Process -Filter "ProcessId = {pids[0]}").CommandLine'
    result = subprocess.run([str(powershell), '-NoProfile', '-Command', command], capture_output=True, text=True, check=True)
    match = re.search(r'-c\s+(?:"([^"]+)"|(\S+))', result.stdout)
    assert match, result.stdout
    config = json.loads(Path(match.group(1) or match.group(2)).read_text())
    assert config['outbounds'][0]['protocol'] == 'freedom'
    inbound = config['inbounds'][0]
    account = inbound['settings']['accounts'][0]
    username, password = account['user'].encode(), account['pass'].encode()
    with socket.create_connection(('127.0.0.1', inbound['port']), timeout=5) as sock:
        sock.sendall(bytes.fromhex('050102'))
        assert sock.recv(2) == bytes.fromhex('0502')
        sock.sendall(b'\x01' + bytes([len(username)]) + username + bytes([len(password)]) + password)
        assert sock.recv(2) == bytes.fromhex('0100')
    print('PASS: relaunched app owns a working authenticated Xray SOCKS5 tunnel', flush=True)
    return pids[0]

with tempfile.TemporaryDirectory(prefix='importer-restart-') as temp:
    root = Path(temp)
    with zipfile.ZipFile(sys.argv[1]) as archive:
        assert archive.testzip() is None
        assert not any(name.endswith(('profile.json', '.ini', '.log')) for name in archive.namelist())
        archive.extractall(root)
    folder = root / 'Moonlight-VLESS-Import-Portable'
    exe = folder / 'Moonlight.exe'
    recovery_start = '--startup-recovery' in sys.argv[2:]
    if recovery_start: (folder / 'Xray/profile.json').write_text('{invalid-startup-profile')
    profile = root / 'test profile.json'
    profile.write_text(json.dumps({'outbounds': [{'protocol': 'freedom', 'tag': 'test'}]}))
    env = dict(os.environ, PATH=str(Path(os.environ['SystemRoot']) / 'System32') + ';' + os.environ['SystemRoot'],
               QT_FORCE_STDERR_LOGGING='1', QT_ACCESSIBILITY='1')
    for key in ('QTDIR', 'QT_PLUGIN_PATH', 'QT_QPA_PLATFORM_PLUGIN_PATH', 'QML_IMPORT_PATH', 'QML2_IMPORT_PATH'):
        env.pop(key, None)
    try:
        with (root / 'first-launch.log').open('w') as log:
            first = subprocess.Popen([str(exe)], cwd=root, env=env, stdout=log, stderr=log)
            time.sleep(7)
            assert first.poll() is None
            if recovery_start:
                assert not matching_processes(folder / 'Xray/xray.exe')
                assert 'Invalid Xray profile' in (folder / 'logs/tunnel-debug.jsonl').read_text(errors='replace')
                print('PASS: failed startup remains usable for Settings and profile replacement', flush=True)
            direct = '--direct' in sys.argv[2:] or recovery_start
            automatic = '--automatic' in sys.argv[2:]
            ui(first.pid, profile, 'ImportAutomatic' if automatic else ('ImportRestart' if direct else 'ImportLater'))
            assert json.loads((folder / 'Xray/profile.json').read_text())['outbounds'][0]['protocol'] == 'freedom'
            if not direct and not automatic:
                assert first.poll() is None
                assert 'Integrated VLESS/Reality tunnel ready' not in (root / 'first-launch.log').read_text(errors='replace')
                ui(first.pid, profile, 'RestartPending')
            assert first.wait(timeout=40) == 0
        time.sleep(7)
        pids = matching_processes(exe)
        assert len(pids) == 1 and pids[0] != first.pid, pids
        first_core = verify_tunnel(folder / 'Xray/xray.exe')
        if '--recovery' in sys.argv[2:]:
            subprocess.run(['taskkill', '/PID', str(first_core), '/F'], check=True, capture_output=True)
            deadline = time.monotonic() + 30
            while True:
                cores = matching_processes(folder / 'Xray/xray.exe')
                if len(cores) == 1 and cores[0] != first_core:
                    time.sleep(2)
                    verify_tunnel(folder / 'Xray/xray.exe')
                    break
                if time.monotonic() > deadline: raise AssertionError('GUI did not automatically recover Xray')
                time.sleep(.2)
            assert matching_processes(exe) == pids
            print('PASS: real GUI recovers a killed Xray core while keeping the same Moonlight PID', flush=True)
        print('PASS: automatic relaunch creates exactly one new process and activates the imported profile', flush=True)
        old = (folder / 'Xray/profile.json').read_bytes()
        profile.write_text('{invalid-json')
        ui(pids[0], profile, 'ImportInvalid')
        assert (folder / 'Xray/profile.json').read_bytes() == old
        print('PASS: invalid import preserves the active profile', flush=True)
        # Close the validation error so Settings can be used for export.
        if '--diagnostics' in sys.argv[2:]:
            powershell = Path(os.environ['SystemRoot']) / 'System32/WindowsPowerShell/v1.0/powershell.exe'
            # Escape dismisses the Qt validation dialog without quitting the app.
            command = '$ws=New-Object -ComObject WScript.Shell; $ws.AppActivate(' + str(pids[0]) + '); $ws.SendKeys("{ESC}")'
            subprocess.run([str(powershell), '-NoProfile', '-Command', command], check=True, capture_output=True)
            time.sleep(1)
            ui(pids[0], profile, 'ExportReport')
            assert list((folder / 'logs').glob('xray-diagnostic-*.txt'))
            print('PASS: persistent diagnostic logs/report exist in the portable folder', flush=True)
        first_log = (root / 'first-launch.log').read_text(errors='replace')
        for error in ('ReferenceError:', 'TypeError:', 'Binding loop detected for property "implicitWidth"', 'different thread', 'Cannot move to target thread'):
            assert error not in first_log, first_log
        print('PASS: restart dialog has no QML reference/type errors or width-binding loops', flush=True)
    finally:
        for pid in matching_processes(exe): close_gui(pid)
        deadline = time.monotonic() + 15
        while matching_processes(exe) and time.monotonic() < deadline: time.sleep(0.2)
        for pid in matching_processes(exe):
            handle = kernel.OpenProcess(1, False, pid)
            if handle:
                kernel.TerminateProcess(handle, 1)
                kernel.CloseHandle(handle)
