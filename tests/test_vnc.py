import array
import os
import queue
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
HELPER = os.path.join(HERE, "..", "native", "build", "omaremote-vnc")

# A VNC server that stops in the middle of a message held omaremote-vnc inside libvncclient for
# good, deaf to SIGTERM (found while testing omarchy-plugin-marketplace#11853): now a read gives
# up after readTimeout seconds, and an idle session is still not cut.


def fake_server(listener, stall, size=(64, 48), pixels=False, client_data=None, server_events=None):
    """RFB 3.8 without a password; then a partial, empty, or synthetic pixel update."""
    conn, _ = listener.accept()
    if server_events is not None:
        server_events.put("accepted")
    with conn:
        conn.sendall(b"RFB 003.008\n")
        conn.recv(12)
        conn.sendall(b"\x01\x01")  # one security type: none
        conn.recv(1)
        conn.sendall(struct.pack(">I", 0))
        conn.recv(1)  # ClientInit
        pixel = struct.pack(">BBBBHHHBBB3x", 32, 24, 0, 1, 255, 255, 255, 16, 8, 0)
        conn.sendall(struct.pack(">HH", *size) + pixel + struct.pack(">I", 4) + b"fake")
        time.sleep(0.3)  # SetPixelFormat, SetEncodings, the first update request
        if pixels:
            # Padding is deliberately not alpha, including both zero and nonzero values.
            full = b"".join(bytes((17, 34, 51, i % 256)) for i in range(64 * 48))
            patch = bytes((68, 85, 102, 7)) * 6
            conn.sendall(struct.pack(">BxH", 0, 2)
                         + struct.pack(">HHHHi", 0, 0, 64, 48, 0) + full
                         + struct.pack(">HHHHi", 3, 4, 2, 3, 0) + patch)
        elif stall:
            conn.sendall(struct.pack(">BxH", 0, 1) + b"\x00\x00\x00\x00")  # 4 of the 12 header bytes
        else:
            conn.sendall(struct.pack(">BxH", 0, 0))
        conn.settimeout(0.2)
        end = time.monotonic() + 30
        while time.monotonic() < end:
            try:
                data = conn.recv(4096)
                if not data:
                    return
                if client_data is not None:
                    client_data.put(data)
            except socket.timeout:
                pass
            except OSError:
                return


@unittest.skipUnless(os.path.exists(HELPER), "needs the native build (ninja -C native/build)")
class VncSession(unittest.TestCase):
    def start(self, stall, size=(64, 48), pixels=False, client_data=None, config="", server_events=None):
        d = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, d)
        listener = socket.socket()
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        self.addCleanup(listener.close)
        threading.Thread(target=fake_server,
                         args=(listener, stall, size, pixels, client_data, server_events), daemon=True).start()
        port = listener.getsockname()[1]
        p = subprocess.Popen([HELPER], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             env=dict(os.environ, OMAREMOTE_SOCKET=os.path.join(d, "s")), text=True)
        self.addCleanup(p.stdout.close)
        self.addCleanup(lambda: p.poll() is None and p.kill())
        p.stdin.write(f"host=127.0.0.1\nport={port}\nreadTimeout=2\n" + config)
        p.stdin.close()
        self.socket_path = os.path.join(d, "s")
        return p

    def mac_helper(self, mode="normal"):
        directory = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, directory)
        script = os.path.join(directory, "helper.py")
        self.mac_log = os.path.join(directory, "keys")
        with open(script, "w") as f:
            f.write('''import os, sys, time
mode, path = sys.argv[1:]
with open(path, "a", buffering=1) as log:
    log.write("pid %d\\n" % os.getpid())
    if mode == "missing":
        print("Install omaremote-mac-input on SSH PATH", file=sys.stderr, flush=True)
        sys.exit(127)
    if mode == "silent":
        time.sleep(30)
    if mode == "delayed":
        time.sleep(0.8)
    print("ready 2" if mode == "bad" else "ready 1", flush=True)
    if mode in ("stalled", "stubborn"):
        if mode == "stubborn":
            import signal
            signal.signal(signal.SIGTERM, signal.SIG_IGN)
        time.sleep(30)
    for line in sys.stdin:
        log.write(line)
        if mode == "death" and line.startswith("key "):
            print("Accessibility permission denied", file=sys.stderr, flush=True)
            sys.exit(1)
        if line == "ping\\n" and mode != "no-pong":
            print("pong", flush=True)
    log.write("eof\\n")
''')
        return "".join(f"macInputArg={arg}\n" for arg in [sys.executable, script, mode, self.mac_log])

    def attach(self):
        ui = socket.socket(socket.AF_UNIX)
        self.addCleanup(ui.close)
        ui.settimeout(5)
        deadline = time.monotonic() + 5
        while True:
            try:
                ui.connect(self.socket_path)
                return ui
            except (FileNotFoundError, ConnectionRefusedError):
                if time.monotonic() >= deadline:
                    self.fail("VNC session did not open its UI socket")
                time.sleep(0.01)

    def connected(self, ui):
        received = b""
        while b"state connected\n" not in received:
            data = ui.recv(4096)
            self.assertTrue(data, "session closed before connecting")
            received += data

    def wait_mac(self, expected):
        deadline = time.monotonic() + 7
        while time.monotonic() < deadline:
            try:
                with open(self.mac_log) as f:
                    text = f.read()
                if expected in text:
                    return text
            except FileNotFoundError:
                pass
            time.sleep(0.02)
        self.fail(f"helper did not receive {expected!r}")

    def rfb_messages(self, incoming):
        pending = b""
        while True:
            try:
                pending += incoming.get(timeout=0.4)
            except queue.Empty:
                break
        messages = []
        while pending:
            kind = pending[0]
            if kind == 0:
                length = 20
            elif kind == 2:
                length = 4 + 4 * struct.unpack_from(">H", pending, 2)[0]
            elif kind == 3:
                length = 10
            elif kind == 4:
                length = 8
            elif kind == 5:
                length = 6
            elif kind == 6:
                length = 8 + struct.unpack_from(">I", pending, 4)[0]
            else:
                self.fail(f"unexpected RFB message {kind}")
            self.assertGreaterEqual(len(pending), length)
            messages.append(pending[:length])
            pending = pending[length:]
        return messages

    def test_mac_helper_exclusive_physical_keys_cad_and_blur(self):
        incoming = queue.Queue()
        p = self.start(False, client_data=incoming, config=self.mac_helper())
        ui = self.attach()
        self.connected(ui)
        ui.sendall(b"key 1 30 0\nkey 0 30\ncad\nkey 1 29 65507\n")
        self.wait_mac("key 1 29\nkey 1 56\nkey 1 111\nkey 0 111\nkey 0 56\nkey 0 29\nkey 1 29\n")
        ui.sendall(b"blur\nmouse 7 8\nclip aGVsbG8=\n")
        self.wait_mac("key 1 29\nrelease\n")
        messages = self.rfb_messages(incoming)
        self.assertNotIn(4, [m[0] for m in messages])
        self.assertIn(5, [m[0] for m in messages])
        self.assertIn(6, [m[0] for m in messages])
        text = self.wait_mac("key 0 30\n")
        self.assertIn("key 1 30\nkey 0 30\n", text)
        ui.sendall(b"key 1 42 65505\n")
        self.wait_mac("key 1 42\n")
        ui.close()
        self.wait_mac("key 1 42\nrelease\n")
        p.terminate()
        self.assertEqual(p.wait(5), 11)
        self.wait_mac("eof\n")

    def test_mac_helper_view_only_injects_no_keyboard(self):
        incoming = queue.Queue()
        p = self.start(False, client_data=incoming, config=self.mac_helper() + "viewOnly=1\n")
        ui = self.attach()
        self.connected(ui)
        ui.sendall(b"key 1 30 97\nkey 0 30\ncad\n")
        time.sleep(0.1)
        self.assertNotIn(4, [m[0] for m in self.rfb_messages(incoming)])
        p.terminate()
        self.assertEqual(p.wait(5), 11)
        text = self.wait_mac("eof\n")
        self.assertNotIn("key ", text)

    def test_mac_helper_readiness_precedes_connected(self):
        events = queue.Queue()
        p = self.start(False, config=self.mac_helper("delayed"), server_events=events)
        ui = self.attach()
        ui.settimeout(0.2)
        try:
            early = ui.recv(4096)
            self.assertNotIn(b"state connected\n", early)
        except socket.timeout:
            pass
        self.assertTrue(events.empty(), "VNC startup preceded keyboard helper readiness")
        ui.settimeout(5)
        self.connected(ui)
        p.terminate()
        self.assertEqual(p.wait(5), 11)

    def test_mac_helper_missing_bad_or_absent_readiness_fails(self):
        for mode, reason in [("missing", "Install omaremote-mac-input"),
                             ("bad", "unexpected helper response"),
                             ("silent", "timed out waiting for ready 1")]:
            with self.subTest(mode=mode):
                p = self.start(False, config=self.mac_helper(mode))
                self.assertEqual(p.wait(15), 147)
                output = p.stdout.read()
                self.assertIn("Mac input helper:", output)
                self.assertIn(reason, output)
                self.assertNotIn("connected to fake", output)
                with open(self.mac_log) as f:
                    pid = int(f.readline().split()[1])
                with self.assertRaises(ProcessLookupError):
                    os.kill(pid, 0)

    def test_mac_helper_death_fails_without_rfb_fallback(self):
        incoming = queue.Queue()
        p = self.start(False, client_data=incoming, config=self.mac_helper("death"))
        ui = self.attach()
        self.connected(ui)
        ui.sendall(b"key 1 30 97\n")
        self.assertEqual(p.wait(5), 147)
        output = p.stdout.read()
        self.assertIn("Mac input helper:", output)
        self.assertIn("Accessibility permission denied", output)
        self.assertNotIn(4, [m[0] for m in self.rfb_messages(incoming)])

    def test_mac_helper_heartbeat_and_timeout(self):
        p = self.start(False, config=self.mac_helper())
        ui = self.attach()
        self.connected(ui)
        self.wait_mac("ping\n")
        self.assertIsNone(p.poll())
        p.terminate()
        self.assertEqual(p.wait(5), 11)
        p = self.start(False, config=self.mac_helper("no-pong"))
        self.connected(self.attach())
        self.assertEqual(p.wait(18), 147)
        self.assertIn("heartbeat timed out", p.stdout.read())

    def test_mac_helper_stalled_input_is_bounded_and_fails(self):
        incoming = queue.Queue()
        p = self.start(False, client_data=incoming, config=self.mac_helper("stalled"))
        ui = self.attach()
        self.connected(ui)
        # Repeated same physical key avoids relying on unsupported key identities.
        try:
            ui.sendall(b"key 1 30 97\n" * 20000)
        except BrokenPipeError:
            pass
        self.assertEqual(p.wait(10), 147)
        self.assertIn("Mac input helper:", p.stdout.read())
        self.assertNotIn(4, [m[0] for m in self.rfb_messages(incoming)])

    def test_mac_helper_stubborn_child_is_terminated_and_reaped(self):
        p = self.start(False, config=self.mac_helper("stubborn"))
        self.connected(self.attach())
        with open(self.mac_log) as f:
            pid = int(f.readline().split()[1])
        started = time.monotonic()
        p.terminate()
        self.assertEqual(p.wait(5), 11)
        self.assertLess(time.monotonic() - started, 3)
        with self.assertRaises(ProcessLookupError):
            os.kill(pid, 0)

    def test_framebuffer_padding_is_opaque_without_changing_rgb(self):
        p = self.start(stall=False, pixels=True)
        ui = socket.socket(socket.AF_UNIX)
        self.addCleanup(ui.close)
        ui.settimeout(5)
        deadline = time.monotonic() + 5
        while True:
            try:
                ui.connect(self.socket_path)
                break
            except (FileNotFoundError, ConnectionRefusedError):
                if time.monotonic() >= deadline:
                    self.fail("VNC session did not open its UI socket")
                time.sleep(0.01)
        received = b""
        frame_fd = None
        while b"damage " not in received or frame_fd is None:
            data, ancillary, _, _ = ui.recvmsg(4096, socket.CMSG_SPACE(4 * 4))
            self.assertTrue(data, "session closed before sending a frame")
            received += data
            for level, kind, payload in ancillary:
                if level == socket.SOL_SOCKET and kind == socket.SCM_RIGHTS:
                    fds = array.array("i")
                    fds.frombytes(payload[:len(payload) - len(payload) % fds.itemsize])
                    for fd in fds:
                        self.addCleanup(os.close, fd)
                        frame_fd = fd
        self.assertIn(b"frame 64 48 256\n", received)
        actual = os.pread(frame_fd, 64 * 48 * 4, 0)
        expected = bytearray(bytes((17, 34, 51, 255)) * (64 * 48))
        for y in range(4, 7):
            offset = (y * 64 + 3) * 4
            expected[offset:offset + 8] = bytes((68, 85, 102, 255)) * 2
        self.assertEqual(actual, expected)
        p.terminate()
        self.assertEqual(p.wait(5), 11)

    def test_hangul_and_hanja_key_events_reach_rfb(self):
        client_data = queue.Queue()
        p = self.start(stall=False, client_data=client_data)
        ui = socket.socket(socket.AF_UNIX)
        self.addCleanup(ui.close)
        ui.settimeout(5)
        deadline = time.monotonic() + 5
        while True:
            try:
                ui.connect(self.socket_path)
                break
            except (FileNotFoundError, ConnectionRefusedError):
                if time.monotonic() >= deadline:
                    self.fail("VNC session did not open its UI socket")
                time.sleep(0.01)
        received = b""
        while b"state connected\n" not in received:
            data = ui.recv(4096)
            self.assertTrue(data, "session closed before connecting")
            received += data

        # Releases contain only the evdev code; the backend must retain the press keysym.
        ui.sendall(b"key 1 122 65329\nkey 0 122\nkey 1 123 65332\nkey 0 123\n")
        expected = [(1, 0xff31), (0, 0xff31), (1, 0xff34), (0, 0xff34)]
        events = []
        pending = b""
        deadline = time.monotonic() + 5
        while len(events) < len(expected):
            remaining = deadline - time.monotonic()
            self.assertGreater(remaining, 0, "timed out waiting for RFB key events")
            try:
                pending += client_data.get(timeout=remaining)
            except queue.Empty:
                self.fail("timed out waiting for RFB key events")
            while pending:
                kind = pending[0]
                if kind == 0:  # SetPixelFormat
                    length = 20
                elif kind == 2:  # SetEncodings
                    if len(pending) < 4:
                        break
                    length = 4 + 4 * struct.unpack_from(">H", pending, 2)[0]
                elif kind == 3:  # FramebufferUpdateRequest
                    length = 10
                elif kind == 4:  # KeyEvent
                    length = 8
                elif kind == 5:  # PointerEvent
                    length = 6
                elif kind == 6:  # ClientCutText
                    if len(pending) < 8:
                        break
                    length = 8 + struct.unpack_from(">I", pending, 4)[0]
                else:
                    self.fail(f"unexpected RFB client message type {kind}")
                if len(pending) < length:
                    break
                if kind == 4:
                    events.append((pending[1], struct.unpack_from(">I", pending, 4)[0]))
                pending = pending[length:]
        self.assertEqual(events, expected)
        p.terminate()
        self.assertEqual(p.wait(5), 11)

    def test_a_server_stalling_mid_message_ends_the_session(self):
        p = self.start(stall=True)
        started = time.monotonic()
        try:
            rc = p.wait(15)
        except subprocess.TimeoutExpired:
            self.fail("omaremote-vnc still waits for the rest of the message")
        log = p.stdout.read()
        self.assertIn("connected to fake", log)
        self.assertEqual(rc, 147, log)
        self.assertLess(time.monotonic() - started, 10)

    def test_a_desktop_past_8192_a_side_ends_the_session(self):
        # 65535x65535 would be a 17 GB shared framebuffer, mapped by the manager window.
        p = self.start(stall=False, size=(65535, 65535))
        try:
            rc = p.wait(15)
        except subprocess.TimeoutExpired:
            self.fail("omaremote-vnc took a 65535x65535 desktop")
        log = p.stdout.read()
        self.assertIn("larger than OMARemote shows", log)
        self.assertNotIn("connected to fake", log)
        self.assertNotEqual(rc, 0)

    def test_an_idle_session_is_not_cut(self):
        p = self.start(stall=False)
        time.sleep(5)  # more than twice readTimeout
        self.assertIsNone(p.poll(), p.stdout.read() if p.poll() is not None else "")
        p.terminate()
        self.assertEqual(p.wait(5), 11)


if __name__ == "__main__":
    unittest.main()
