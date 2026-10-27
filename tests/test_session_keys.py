import json
import os
import re
import shutil
import subprocess
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

class CaptureToggle(unittest.TestCase):
    """The one-chord capture toggle (ctrl+alt+g): what Keymap advertises, SessionView runs, the
    native side answers and the bars hint must stay one and the same chord, and it must not be a
    chord anything else uses."""

    @staticmethod
    def text(name):
        with open(os.path.join(UI, name), encoding="utf-8") as f:
            return f.read()

    def test_the_chord_is_free_everywhere(self):
        # ctrl+alt+t is taken (an own window's to-a-tab), so the toggle has its own chord, and
        # no second row ever binds it.
        self.assertIn('"ctrl+alt+t"', self.text("Keymap.qml"))  # still the own-window chord
        rows = re.findall(r'keys: \[([^\]]*)\]', self.text("Keymap.qml"))
        used = [spec for row in rows for spec in re.findall(r'"([^"]+)"', row)]
        self.assertEqual(used.count("ctrl+alt+g"), 1, "exactly one binding: the toggle")
        self.assertEqual(used.count("ctrl+alt+t"), 1, "the own-window chord keeps its place")

    def test_session_view_toggles_both_ways_on_the_toggle_chord(self):
        qml = self.text("SessionView.qml")
        self.assertIn("event.key === Qt.Key_G) view.shortcutsCaptured ? view.releaseShortcuts()"
                      " : view.rearmShortcuts()", qml)
        # The plain release chord stays release-only, as before.
        self.assertIn("event.key === Qt.Key_Escape && view.shortcutsCaptured) view.releaseShortcuts()", qml)

    def test_the_native_side_answers_both_ways(self):
        with open(os.path.join(HERE, "..", "native", "plugin", "rdpview.h"), encoding="utf-8") as f:
            header = f.read()
        self.assertIn("Q_INVOKABLE void rearmShortcuts();", header)
        self.assertIn('"released" the release key or chord gave them', header)
        with open(os.path.join(HERE, "..", "native", "plugin", "rdpview.cpp"), encoding="utf-8") as f:
            cpp = f.read()
        self.assertIn('next = m_released && asksForShortcuts() ? QStringLiteral("released")'
                      ' : QStringLiteral("ineligible")', cpp)
        self.assertRegex(cpp, r"void RdpView::rearmShortcuts\(\)\s*\{[^}]*m_released = false")

    def test_the_bars_and_helps_tell_the_chord(self):
        for name in ("Body.qml", "SessionWindow.qml"):
            self.assertIn('["ctrl+alt+g", "capture"]', self.text(name), name)
        self.assertIn("⌨ released", self.text("StatusBar.qml"))
        for name in ("Editor.qml", "GroupEditor.qml", "SettingsPanel.qml", "Keymap.qml"):
            self.assertIn("ctrl+alt+g", self.text(name), name)

if __name__ == "__main__":
    unittest.main()
