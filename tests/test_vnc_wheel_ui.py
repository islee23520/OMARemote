import os
import re
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))


def wheel_input_block(name):
    with open(os.path.join(HERE, "..", "ui", name), encoding="utf-8") as f:
        source = f.read()
    start = source.index('label: "Wheel sensitivity"')
    end = source.index("\n                FormRow {", start)
    block = source[start:end]
    match = re.search(r"Input \{(.*?)\n\s*\}", block, re.S)
    if not match:
        raise AssertionError(f"missing Wheel sensitivity Input in {name}")
    return match.group(1)


class WheelScaleUi(unittest.TestCase):
    def test_connection_and_group_editors_use_the_backend_step(self):
        for name in ("Editor.qml", "GroupEditor.qml"):
            with self.subTest(file=name):
                handler = wheel_input_block(name)
                self.assertIn("n >= 0.25 && n <= 8", handler)
                self.assertIn("Math.abs(n * 1000 - Math.round(n * 1000)) < 1e-9", handler)
                self.assertIn('root.set("vncWheelScale", n)', handler)

    def test_valid_decimal_float_tails_are_accepted_by_backend_grid(self):
        for text in ("1.001", "1.003"):
            value = float(text)
            self.assertLess(abs(value * 1000 - round(value * 1000)), 1e-9)
        value = 1.2345
        self.assertGreaterEqual(abs(value * 1000 - round(value * 1000)), 1e-9)


if __name__ == "__main__":
    unittest.main()
