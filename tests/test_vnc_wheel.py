import array
import importlib.machinery
import importlib.util
import os
import queue
import shutil
import socket
import struct
import subprocess
import tempfile
import threading
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
HELPER = os.environ.get("OMAREMOTE_VNC", os.path.join(HERE, "..", "native", "build", "omaremote-vnc"))
_loader = importlib.machinery.SourceFileLoader("session", os.path.join(HERE, "..", "bin", "omaremote-session"))
_spec = importlib.util.spec_from_loader("session", _loader)
session = importlib.util.module_from_spec(_spec)
_loader.exec_module(session)


def conn(**kw):
    return dict({"id": "v1", "name": "VNC", "host": "pc.lan", "protocol": "vnc"}, **kw)


def fake_server(listener, input_packets):
    conn_sock, _ = listener.accept()
    with conn_sock:
        conn_sock.sendall(b"RFB 003.008\n")
        conn_sock.recv(12)
        conn_sock.sendall(b"\x01\x01")
        conn_sock.recv(1)
        conn_sock.sendall(struct.pack(">I", 0))
        conn_sock.recv(1)
        pixel = struct.pack(">BBBBHHHBBB3x", 32, 24, 0, 1, 255, 255, 255, 16, 8, 0)
        conn_sock.sendall(struct.pack(">HH", 64, 48) + pixel + struct.pack(">I", 4) + b"fake")
        time.sleep(0.35)  # consume initial SetPixelFormat/SetEncodings/update request before input
        conn_sock.sendall(struct.pack(">BxH", 0, 0))
        conn_sock.settimeout(0.2)
        until = time.monotonic() + 30
        while time.monotonic() < until:
            try:
                data = conn_sock.recv(4096)
                if not data:
                    return
                input_packets.put(data)
            except socket.timeout:
                pass
            except OSError:
                return


def pointer_masks(chunks, count):
    """Decode client RFB messages and return exactly count PointerEvent masks."""
    data = b"".join(chunks)
    masks = []
    pos = 0
    while pos < len(data):
        kind = data[pos]
        if kind == 0:  # SetPixelFormat
            length = 20
        elif kind == 2:  # SetEncodings
            if len(data) - pos < 4:
                break
            length = 4 + struct.unpack_from(">H", data, pos + 2)[0] * 4
        elif kind == 3:  # FramebufferUpdateRequest
            length = 10
        elif kind == 4:  # KeyEvent
            length = 8
        elif kind == 5:  # PointerEvent
            length = 6
        elif kind == 6:  # ClientCutText
            if len(data) - pos < 8:
                break
            length = 8 + struct.unpack_from(">I", data, pos + 4)[0]
        else:
            raise AssertionError(f"unexpected RFB client message {kind} at byte {pos}")
        if len(data) - pos < length:
            break
        if kind == 5:
            masks.append(data[pos + 1])
            if len(masks) == count:
                return masks
        pos += length
    return masks


@unittest.skipUnless(os.path.exists(HELPER), "needs the native build (ninja -C native/build)")
class WheelScale(unittest.TestCase):
    def test_connection_config_defaults_roundtrip_and_bounds(self):
        self.assertIn("wheelScale=1.0", session.build_vnc_config(conn()))
        self.assertIn("wheelScale=2.5", session.build_vnc_config(conn(vncWheelScale=2.5)))
        for value in (0.25, 8, 1.25):
            self.assertIsNone(session.odd_types(conn(vncWheelScale=value)))
        for value in (0.249, 8.001, True, "2", float("inf"), float("nan")):
            self.assertEqual(session.odd_types(conn(vncWheelScale=value)), "vncWheelScale")

    def test_group_inheritance_and_connection_override(self):
        data = {"groups": {"Desk": {"settings": {"vncWheelScale": 2.5}}}}
        self.assertEqual(session.resolve(conn(group="Desk"), data)["vncWheelScale"], 2.5)
        self.assertEqual(session.resolve(conn(group="Desk", vncWheelScale=0.5), data)["vncWheelScale"], 0.5)

    def test_connection_and_group_exports_keep_the_setting(self):
        document = session.portable({"connections": [conn(vncWheelScale=2.5)],
                                     "groups": {"Desk": {"settings": {"vncWheelScale": 1.5}}}}, "test")
        self.assertEqual(document["connections"][0]["vncWheelScale"], 2.5)
        self.assertEqual(document["groups"]["Desk"]["settings"]["vncWheelScale"], 1.5)

    def test_default_and_scaled_inputs_produce_expected_rfb_wire(self):
        for scale, expected in ((1, [1, 9, 1, 65, 1]),
                                (2, [1, 9, 1, 65, 1, 9, 1, 65, 1]),
                                (0.5, [1, 9, 1, 65, 1])):
            with self.subTest(scale=scale):
                d = tempfile.mkdtemp()
                self.addCleanup(shutil.rmtree, d)
                listener = socket.socket()
                listener.bind(("127.0.0.1", 0))
                listener.listen(1)
                self.addCleanup(listener.close)
                packets = queue.Queue()
                threading.Thread(target=fake_server, args=(listener, packets), daemon=True).start()
                port = listener.getsockname()[1]
                proc = subprocess.Popen([HELPER], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.STDOUT, env=dict(os.environ,
                                        OMAREMOTE_SOCKET=os.path.join(d, "s")), text=True)
                self.addCleanup(proc.stdout.close)
                self.addCleanup(lambda: proc.poll() is None and proc.kill())
                config = f"host=127.0.0.1\nport={port}\nreadTimeout=2\n"
                if scale != 1:
                    config += f"wheelScale={scale}\n"
                proc.stdin.write(config)
                proc.stdin.close()
                socket_path = os.path.join(d, "s")
                ui = socket.socket(socket.AF_UNIX)
                self.addCleanup(ui.close)
                ui.settimeout(5)
                deadline = time.monotonic() + 5
                while True:
                    try:
                        ui.connect(socket_path)
                        break
                    except (FileNotFoundError, ConnectionRefusedError):
                        if time.monotonic() >= deadline:
                            self.fail("VNC session did not open its UI socket")
                        time.sleep(0.01)
                received = b""
                while b"state connected\n" not in received:
                    received += ui.recv(4096)
                ui.sendall(b"button 1 1 10 10\n")
                if scale == 0.5:
                    for _ in range(4):
                        ui.sendall(b"wheel 30 -30 10 10\n")
                    for _ in range(4):
                        ui.sendall(b"wheel -30 30 10 10\n")
                    for _ in range(8):
                        ui.sendall(b"wheel 30 -30 10 10\n")
                else:
                    for _ in range(4):
                        ui.sendall(b"wheel 30 -30 10 10\n")
                wire_chunks = []
                until = time.monotonic() + 5
                while len(pointer_masks(wire_chunks, len(expected))) < len(expected):
                    remaining = until - time.monotonic()
                    self.assertGreater(remaining, 0, "timed out waiting for RFB PointerEvents")
                    try:
                        wire_chunks.append(packets.get(timeout=remaining))
                    except queue.Empty:
                        self.fail("timed out waiting for RFB PointerEvents")
                self.assertEqual(pointer_masks(wire_chunks, len(expected)), expected)
                proc.terminate()
                self.assertEqual(proc.wait(5), 11)


if __name__ == "__main__":
    unittest.main()
