"""Windows lifecycle regression for the standalone IntegratedTunnel harness."""
import ctypes
from ctypes import wintypes
import os
from pathlib import Path
import queue
import re
import subprocess
import sys
import threading

kernel = ctypes.WinDLL("kernel32", use_last_error=True)
kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
kernel.OpenProcess.restype = wintypes.HANDLE
kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
kernel.WaitForSingleObject.restype = wintypes.DWORD
kernel.CloseHandle.argtypes = [wintypes.HANDLE]

def start(exe):
    env = dict(os.environ, QT_FORCE_STDERR_LOGGING="1")
    process = subprocess.Popen([exe, "--hold"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               text=True, env=env)
    lines = queue.Queue()
    def read():
        for line in process.stdout: lines.put(line)
        lines.put(None)
    threading.Thread(target=read, daemon=True).start()
    child = None
    try:
        while True:
            line = lines.get(timeout=15)
            if line is None: raise RuntimeError(f"Manager exited early: {process.poll()}")
            match = re.search(r"core PID: (\d+)", line)
            if match: child = int(match.group(1))
            if "MANAGER_READY" in line:
                assert child
                return process, child
    except BaseException:
        process.kill(); process.wait()
        raise

exe = sys.argv[1]
parent, pid = start(exe)
handle = kernel.OpenProcess(0x100000, False, pid)
assert handle
try:
    parent.kill(); parent.wait(timeout=5)
    assert kernel.WaitForSingleObject(handle, 5000) == 0, "Xray survived parent termination"
    print("PASS: Windows job kills Xray even if Moonlight crashes or is force-closed")
finally:
    kernel.CloseHandle(handle)

parent, pid = start(exe)
try:
    subprocess.run(["taskkill", "/PID", str(pid), "/F"], check=True, capture_output=True)
    assert parent.wait(timeout=8) == 1, "Manager did not exit on core failure"
    print("PASS: unexpected core exit stops application without direct fallback")
finally:
    if parent.poll() is None: parent.kill(); parent.wait()

# Only modify the dedicated TEST harness profile, restoring it in all cases.
profile = Path(exe).parent / "Xray" / "profile.json"
original = profile.read_bytes()
try:
    profile.write_text("{malformed")
    result = subprocess.run([exe], capture_output=True, timeout=8)
    assert result.returncode == 1 and b"Invalid Xray profile" in result.stderr
    print("PASS: malformed profile fails before networking")
finally:
    profile.write_bytes(original)
