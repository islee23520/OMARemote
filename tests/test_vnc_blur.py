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
HELPER = os.path.join(HERE, "..", "native", "build", "omaremote-vnc")


def read_exact(conn, size):
    data = b""
    while len(data) < size:
        part = conn.recv(size - len(data))
        if not part:
            raise EOFError("RFB client closed the connection")
        data += part
    return data


def wire_server(listener, events):
    """A local RFB 3.8 server that records complete client input messages."""
    try:
        conn, _ = listener.accept()
        with conn:
            conn.settimeout(10)
            conn.sendall(b"RFB 003.008\n")
            read_exact(conn, 12)
            conn.sendall(b"\x01\x01")  # Security type: None.
            read_exact(conn, 1)
            conn.sendall(struct.pack(">I", 0))
            read_exact(conn, 1)  # ClientInit.
            pixel = struct.pack(">BBBBHHHBBB3x", 32, 24, 0, 1, 255, 255, 255, 16, 8, 0)
            conn.sendall(struct.pack(">HH", 64, 48) + pixel + struct.pack(">I", 4) + b"fake")
            while True:
                kind = read_exact(conn, 1)[0]
                if kind == 0:  # SetPixelFormat
                    read_exact(conn, 19)
                elif kind == 2:  # SetEncodings
                    header = read_exact(conn, 3)
                    read_exact(conn, 4 * struct.unpack(">xH", header)[0])
                elif kind == 3:  # FramebufferUpdateRequest
                    read_exact(conn, 9)
                elif kind == 4:  # KeyEvent
                    down, key = struct.unpack(">B2xI", read_exact(conn, 7))
                    events.put(("key", down, key))
                elif kind == 5:  # PointerEvent
                    mask, x, y = struct.unpack(">BHH", read_exact(conn, 5))
                    events.put(("pointer", mask, x, y))
                elif kind == 6:  # ClientCutText
                    length = struct.unpack(">3xI", read_exact(conn, 7))[0]
                    read_exact(conn, length)
                else:
                    raise ValueError(f"unexpected RFB client message type {kind}")
    except (EOFError, OSError, ValueError) as exc:
        events.put(exc)


@unittest.skipUnless(os.path.exists(HELPER), "needs the native build (ninja -C native/build)")
class VncBlurWire(unittest.TestCase):
    def setUp(self):
        directory = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, directory)
        listener = socket.socket()
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        self.addCleanup(listener.close)
        self.events = queue.Queue()
        threading.Thread(target=wire_server, args=(listener, self.events), daemon=True).start()
        path = os.path.join(directory, "s")
        process = subprocess.Popen([HELPER], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, text=True,
                                   env=dict(os.environ, OMAREMOTE_SOCKET=path))
        self.addCleanup(process.stdout.close)
        self.addCleanup(self.stop_process, process)
        process.stdin.write(f"host=127.0.0.1\nport={listener.getsockname()[1]}\nreadTimeout=2\n")
        process.stdin.close()
        self.ui = socket.socket(socket.AF_UNIX)
        self.addCleanup(self.ui.close)
        self.ui.settimeout(5)
        deadline = time.monotonic() + 5
        while True:
            try:
                self.ui.connect(path)
                break
            except (FileNotFoundError, ConnectionRefusedError):
                if time.monotonic() >= deadline:
                    self.fail("VNC session did not open its UI socket")
                time.sleep(0.01)
        received = b""
        while b"state connected\n" not in received:
            data = self.ui.recv(4096)
            self.assertTrue(data, "session closed before connecting")
            received += data

    @staticmethod
    def stop_process(process):
        if process.poll() is None:
            process.kill()
        process.wait()

    def expect_events(self, expected):
        for event in expected:
            try:
                actual = self.events.get(timeout=5)
            except queue.Empty:
                self.fail(f"timed out waiting for {event!r}")
            if isinstance(actual, Exception):
                self.fail(f"fake RFB server failed: {actual}")
            self.assertEqual(actual, event)

    def test_blur_releases_drag_at_last_position_and_move_stays_up(self):
        self.ui.sendall(b"button 1 1 12 13\nblur\nmouse 20 21\nkey 1 30 97\n")
        # The key is a wire-order barrier: an extra pointer event cannot hide after the expected ones.
        self.expect_events([("pointer", 1, 12, 13), ("pointer", 0, 12, 13),
                            ("pointer", 0, 20, 21), ("key", 1, 97)])

    def test_blur_releases_all_buttons_once_without_lifting_keys(self):
        self.ui.sendall(b"key 1 30 97\nbutton 1 1 10 11\nbutton 2 1 15 16\n"
                        b"mouse 22 23\nblur\nblur\nkey 0 30\nmouse 24 25\nkey 1 31 115\n")
        self.expect_events([("key", 1, 97), ("pointer", 1, 10, 11),
                            ("pointer", 5, 15, 16), ("pointer", 5, 22, 23),
                            ("pointer", 0, 22, 23), ("key", 0, 97),
                            ("pointer", 0, 24, 25), ("key", 1, 115)])

    def test_blur_uses_bounded_coordinates(self):
        self.ui.sendall(b"button 1 1 -10 1000\nblur\nmouse 999999 -5\nkey 1 30 97\n")
        self.expect_events([("pointer", 1, 0, 47), ("pointer", 0, 0, 47),
                            ("pointer", 0, 63, 0), ("key", 1, 97)])

    def test_blur_uses_last_actual_wheel_click_and_preserves_residue(self):
        self.ui.sendall(b"button 1 1 10 11\nwheel 120 0 25 26\n"
                        b"wheel 60 0 40 41\nblur\nwheel 60 0 30 31\nkey 1 30 97\n")
        self.expect_events([("pointer", 1, 10, 11), ("pointer", 9, 25, 26),
                            ("pointer", 1, 25, 26), ("pointer", 0, 25, 26),
                            ("pointer", 8, 30, 31), ("pointer", 0, 30, 31),
                            ("key", 1, 97)])


if __name__ == "__main__":
    unittest.main()
