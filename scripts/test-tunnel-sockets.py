"""Run the production C socket regression harness against a fragmented SOCKS relay."""
import socket
import struct
import subprocess
import sys
import threading
import time

def exact(s, n):
    data = b""
    while len(data) < n:
        part = s.recv(n - len(data))
        if not part: raise EOFError()
        data += part
    return data

class Relay:
    def __init__(self):
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen()
        self.port = self.listener.getsockname()[1]
        self.associations = 0
        self.live = 0
        self.errors = []
        self.lock = threading.Lock()
        self.stop = threading.Event()
        threading.Thread(target=self.accept, daemon=True).start()

    def accept(self):
        while not self.stop.is_set():
            try: s, _ = self.listener.accept()
            except OSError: break
            threading.Thread(target=self.client, args=(s,), daemon=True).start()

    def fragment(self, s, data):
        for b in data:
            s.sendall(bytes([b]))
            time.sleep(0.0001)

    def client(self, s):
        try:
            with s:
                s.settimeout(10)
                greeting = exact(s, 2)
                assert greeting[0] == 5
                methods = exact(s, greeting[1])
                assert 2 in methods
                self.fragment(s, b"\x05\x02")
                assert exact(s, 1) == b"\x01"
                username = exact(s, exact(s, 1)[0])
                password = exact(s, exact(s, 1)[0])
                valid = username == b"testuser" and password == b"testpass"
                self.fragment(s, b"\x01" + bytes([0 if valid else 1]))
                if not valid: return
                request = exact(s, 4)
                assert request[:1] == b"\x05"
                if request[3] == 1: address = exact(s, 4)
                elif request[3] == 4: address = exact(s, 16)
                else: address = exact(s, exact(s, 1)[0])
                port = struct.unpack("!H", exact(s, 2))[0]
                if request[1] == 1:
                    if port == 1:
                        self.fragment(s, b"\x05\x05\x00\x01" + b"\x00" * 6)
                        return
                    self.fragment(s, b"\x05\x00\x00\x01" + b"\x00" * 6)
                    data = s.recv(4096)
                    assert data.startswith(b"OPTIONS ")
                    s.sendall(b"RTSP/1.0 200 OK\r\nCSeq: 1\r\n\r\n")
                    return
                assert request[1] == 3
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
                    udp.bind(("127.0.0.1", 0))
                    udp.settimeout(0.1)
                    self.fragment(s, b"\x05\x00\x00\x01" + socket.inet_aton("127.0.0.1") + struct.pack("!H", udp.getsockname()[1]))
                    with self.lock:
                        self.associations += 1
                        self.live += 1
                    done = threading.Event()
                    def watch():
                        try:
                            while s.recv(1): pass
                        except (OSError, EOFError): pass
                        done.set()
                    threading.Thread(target=watch, daemon=True).start()
                    try:
                        while not done.is_set():
                            try: data, app = udp.recvfrom(65535)
                            except socket.timeout: continue
                            assert data[:4] == b"\x00\x00\x00\x01"
                            assert data[4:8] == socket.inet_aton("203.0.113.10")
                            payload = data[10:]
                            if payload == b"malformed":
                                udp.sendto(b"bad", app)
                                udp.sendto(b"\x00\x00\x01\x01" + data[4:], app)
                                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as spoof:
                                    spoof.sendto(data, app)
                            if payload == b"oversize": data = data[:10] + b"x" * 2000
                            udp.sendto(data, app)
                    finally:
                        with self.lock: self.live -= 1
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError, EOFError): pass
        except Exception as e:
            self.errors.append(repr(e))

if __name__ == "__main__":
    relay = Relay()
    try:
        result = subprocess.run([sys.argv[1], str(relay.port)], timeout=90)
        time.sleep(0.5)
        assert result.returncode == 0, f"Harness returned {result.returncode}"
        assert not relay.errors, relay.errors
        assert relay.associations == 74, f"Duplicate or missing association: {relay.associations}"
        assert relay.live == 0, f"Leaked TCP control sockets: {relay.live}"
        print("PASS: exactly one UDP association per socket; no leaked control connections")
    finally:
        relay.stop.set()
        relay.listener.close()
