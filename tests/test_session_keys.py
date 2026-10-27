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

if __name__ == "__main__":
    unittest.main()
