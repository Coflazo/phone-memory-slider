"""Checks for tools/tokens_to_qml.py. Run: python -m unittest discover -s tools/tests"""

import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools" / "tokens_to_qml.py"
THEME = ROOT / "desktop" / "qml" / "Theme.qml"


def run(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, str(SCRIPT), *args], capture_output=True, text=True)


class TokensToQml(unittest.TestCase):
    def test_generation_is_deterministic(self):
        with tempfile.TemporaryDirectory() as directory:
            first, second = Path(directory, "a.qml"), Path(directory, "b.qml")
            self.assertEqual(run("--theme", str(first)).returncode, 0)
            self.assertEqual(run("--theme", str(second)).returncode, 0)
            self.assertEqual(first.read_bytes(), second.read_bytes())

    def test_committed_theme_matches_tokens(self):
        result = run("--check")
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_check_fails_on_a_hand_edited_copy(self):
        with tempfile.TemporaryDirectory() as directory:
            copy = Path(directory, "Theme.qml")
            shutil.copyfile(THEME, copy)
            copy.write_text(copy.read_text().replace("#FF5A36", "#FF0000"), newline="\n")
            result = run("--check", "--theme", str(copy))
            self.assertEqual(result.returncode, 1)
            self.assertIn("tokens_to_qml.py", result.stderr)

    def test_check_accepts_crlf_checkouts(self):
        with tempfile.TemporaryDirectory() as directory:
            copy = Path(directory, "Theme.qml")
            copy.write_bytes(THEME.read_bytes().replace(b"\r\n", b"\n").replace(b"\n", b"\r\n"))
            self.assertEqual(run("--check", "--theme", str(copy)).returncode, 0)

    def test_theme_carries_v2_tokens(self):
        text = THEME.read_text()
        self.assertIn("generated from design/tokens.json, do not edit", text)
        self.assertIn("Qt.styleHints.colorScheme", text)
        for expected in (
            'property color brand: dark ? "#FF5A36" : "#E2461F"',
            'property color remove: dark ? "#FF7658" : "#BD3216"',
            'property color info: dark ? "#9DBCF6" : "#315EAF"',
            "property int radiusControl: 6",
            "property int motionDeckSettle: 260",
            "property int motionReduced: 120",
            "property real deckNextScale: 0.95",
            'property string fontMono: "Geist Mono"',
        ):
            self.assertIn(expected, text)


if __name__ == "__main__":
    unittest.main()
