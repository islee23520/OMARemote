import json
import os
import re
import shutil
import subprocess
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
UI = os.path.join(HERE, "..", "ui")
QML = shutil.which("qml6") or shutil.which("qml")

# The session keys (Settings › Keyboard): Ctrl+Alt, Right Ctrl (a host key, never sent to the remote
# side) or Ctrl+Alt+Shift. Every chord and every hint follows the one chosen.

@unittest.skipUnless(QML, "needs qml6")
class Matching(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        env = dict(os.environ, QT_QPA_PLATFORM="offscreen", QT_FORCE_STDERR_LOGGING="1")
        p = subprocess.run([QML, os.path.join(HERE, "qml", "sessionkeys.qml")], env=env,
                           capture_output=True, text=True, timeout=30)
        found = re.search(r"RESULT (\{.*\})", p.stderr + p.stdout)
        assert found, p.stderr
        cls.r = json.loads(found.group(1))
        assert "error" not in cls.r, cls.r["error"]

    def taken(self, mode):
        return sorted(k for k, v in self.r["chord"][mode].items() if v)

    def test_ctrl_alt_with_or_without_shift(self):
        self.assertEqual(self.taken("ctrlalt"), ["ctrlalt", "ctrlalt+held", "ctrlaltshift", "ctrlaltshift+held"])

    def test_ctrl_alt_shift_only_with_shift(self):
        self.assertEqual(self.taken("ctrlaltshift"), ["ctrlaltshift", "ctrlaltshift+held"])

    def test_right_ctrl_only_while_held_and_ctrl_down(self):
        # Ctrl+Alt alone goes to Windows (AltGr); a missed release without Ctrl takes nothing.
        self.assertEqual(self.taken("rctrl"), ["ctrl+held", "ctrlalt+held", "ctrlaltshift+held"])

    def test_unknown_setting_is_ctrl_alt(self):
        self.assertEqual(self.r["chord"]["bogus"], self.r["chord"]["ctrlalt"])
        self.assertEqual(self.r["chord"]["undefined"], self.r["chord"]["ctrlalt"])

    def test_keys_as_drawn(self):
        self.assertEqual(self.r["spec"], [["ctrl+alt+home", "ctrl+shift+c"], ["rctrl+home", "ctrl+shift+c"],
                                          ["ctrl+alt+shift+home", "ctrl+shift+c"], ["ctrl+alt+home", "ctrl+shift+c"],
                                          ["ctrl+alt+home", "ctrl+shift+c"]])


    # Dispatcher contract only: extracted QML logic does not prove scene-level event delivery.
class CaptureToggle(unittest.TestCase):
    """Execute SessionView's actual chord and Keys dispatcher bodies under QML."""

    @staticmethod
    def text(name):
        with open(os.path.join(UI, name), encoding="utf-8") as f:
            return f.read()

    @unittest.skipUnless(QML, "needs qml6")
    def test_capture_chord_dispatch_boundaries(self):
        source = self.text("SessionView.qml")
        chord_start = source.index("    function chord(event) {")
        chord_end = source.index("\n    // ---------------------------------------------------------------- connecting", chord_start)
        chord = source[chord_start:chord_end].replace("Keymap.", "keymap.")
        handler_start = source.index("        Keys.onPressed: function (event) {", source.index("    RdpView {"))
        handler_end = source.index("\n        Keys.onReleased", handler_start)
        handler = source[handler_start:handler_end]
        handler = handler[handler.index("{") + 1:handler.rfind("}")]
        mode_start = source.index("    readonly property string resolvedCaptureMode:")
        mode_end = source.index("\n    readonly property string captureState:", mode_start)
        mode = source[mode_start:mode_end]
        eligible = re.search(r"    readonly property bool canToggleCapture:.*(?:\n\s+&&.*)*", source).group(0)
        eligible = eligible.replace("Window.active", "windowActive")
        format_url = "file://" + os.path.abspath(os.path.join(UI, "js", "Format.js"))
        qml = '''import QtQuick
import QtQuick.Window
import "%s" as Format
Window {
    id: root
    property bool windowActive: true
    property bool current: true
    property bool inWindow: false
    property bool live: true
    property bool showDesktop: true
    property bool isSsh: false
    property bool hasPrompt: false
    property var prompt: null
    property var session: ({ protocol: "rdp" })
    property var connection: ({ keyCapture: "auto" })
%s
%s
    property var keymap: ({ sessionKeys: "ctrlalt", isHostKey: function(e) { return e.key === Qt.Key_Control && e.host }, isSessionChord: function(e, held) {
        return Format.isSessionChord(this.sessionKeys, !!(e.modifiers & Qt.ControlModifier),
                                     !!(e.modifiers & Qt.AltModifier),
                                     !!(e.modifiers & Qt.ShiftModifier), held)
    } })
    property var calls: []
    property var view: ({ activeFocus: true, state: "connected", captureState: "ineligible",
        shortcutsCaptured: false, toggleShortcuts: function() { calls.push("toggle"); return eligibleResult },
        releaseShortcuts: function() { calls.push("release") }, releaseAllKeys: function() { calls.push("all") },
        sendCtrlAltDel: function() {}, reconnectNow: function() {} })
    property bool eligibleResult: true
    property bool hostHeld: false
    function navigate(x) {}
    function toggleFullscreen() {}
    function togglePinned() {}
    function reconnectRequested() {}
    function hostKey(event, pressed) {
        if (!keymap.isHostKey(event)) return false
        hostHeld = pressed
        return true
    }
    function run(event) { %s }
%s
    Component.onCompleted: {
        function ev(key, repeat) { return { key: key, modifiers: Qt.ControlModifier | Qt.AltModifier,
                                           isAutoRepeat: !!repeat, accepted: false } }
        var out = {}
        var e = ev(Qt.Key_G, false); run(e); out.gAccepted = e.accepted
        e = ev(Qt.Key_G, true); run(e); out.repeatAccepted = e.accepted
        out.callsAfterG = calls.slice(0)
        e = ev(Qt.Key_Escape, false); run(e); out.escAccepted = e.accepted
        out.callsAfterEsc = calls.slice(0)
        eligibleResult = false; e = ev(Qt.Key_G, false); run(e); out.toggleFalseAccepted = e.accepted
        current = false; e = ev(Qt.Key_G, false); run(e); out.currentFalseAccepted = e.accepted
        current = true; view.activeFocus = false; e = ev(Qt.Key_G, false); run(e); out.focusFalseAccepted = e.accepted
        view.activeFocus = true; windowActive = false; e = ev(Qt.Key_Escape, false); run(e); out.ineligibleEscAccepted = e.accepted
        e = ev(Qt.Key_G, false); run(e); out.windowInactiveAccepted = e.accepted
        windowActive = true
        showDesktop = false; e = ev(Qt.Key_G, false); run(e); out.connectingAccepted = e.accepted
        showDesktop = true; hasPrompt = true; e = ev(Qt.Key_G, false); run(e); out.promptAccepted = e.accepted
        hasPrompt = false; session = { protocol: "ssh" }; connection = { keyCapture: "auto" }
        e = ev(Qt.Key_G, false); run(e); out.sshAccepted = e.accepted
        session = { protocol: "rdp" }; connection = { keyCapture: "off" }
        e = ev(Qt.Key_G, false); run(e); out.offAccepted = e.accepted
        connection = { keyCapture: "auto" }; live = false; showDesktop = false
        e = ev(Qt.Key_G, false); run(e); out.endedAccepted = e.accepted
        e = { key: Qt.Key_Control, host: true, modifiers: Qt.ControlModifier, accepted: false }
        run(e); out.hostHeldAfterPress = hostHeld
        hostKey(e, false); out.hostHeldAfterRelease = hostHeld
        live = true; showDesktop = true; keymap.sessionKeys = "rctrl"; hostHeld = true; live = false; showDesktop = false
        e = ev(Qt.Key_G, false); run(e); out.rightCtrlAccepted = e.accepted
        console.log("RESULT " + JSON.stringify(out)); Qt.exit(0)
    }
}''' % (format_url, mode, eligible, handler, chord)

        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "capture.qml")
            with open(path, "w", encoding="utf-8") as f:
                f.write(qml)
            env = dict(os.environ, QT_QPA_PLATFORM="offscreen", QT_FORCE_STDERR_LOGGING="1")
            p = subprocess.run([QML, path], env=env, capture_output=True, text=True, timeout=30)
        found = re.search(r"RESULT (\{.*\})", p.stderr + p.stdout)
        self.assertIsNotNone(found, p.stderr + p.stdout)
        result = json.loads(found.group(1))
        self.assertTrue(result["gAccepted"])
        self.assertTrue(result["repeatAccepted"])
        self.assertEqual(result["callsAfterG"], ["toggle", "all", "all"])
        self.assertTrue(result["escAccepted"])
        self.assertEqual(result["callsAfterEsc"], ["toggle", "all", "all", "release", "all"])
        self.assertFalse(result["toggleFalseAccepted"])
        self.assertFalse(result["currentFalseAccepted"])
        self.assertFalse(result["focusFalseAccepted"])
        self.assertFalse(result["ineligibleEscAccepted"])
        for key in ("windowInactiveAccepted", "connectingAccepted", "promptAccepted", "sshAccepted", "offAccepted", "endedAccepted"):
            self.assertFalse(result[key], key)
        self.assertTrue(result["rightCtrlAccepted"])
        self.assertTrue(result["hostHeldAfterPress"])
        self.assertFalse(result["hostHeldAfterRelease"])




if __name__ == "__main__":
    unittest.main()
