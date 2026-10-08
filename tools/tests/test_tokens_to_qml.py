"""Checks for tools/tokens_to_qml.py. Run: python -m unittest discover -s tools/tests"""

import copy
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools" / "tokens_to_qml.py"
THEME = ROOT / "desktop" / "qml" / "Theme.qml"
TOKENS = json.loads((ROOT / "design" / "tokens.json").read_text(encoding="utf-8"))
sys.path.insert(0, str(SCRIPT.parent))
import tokens_to_qml  # noqa: E402


def luminance(hex_color: str) -> float:
    channels = [int(hex_color[i : i + 2], 16) / 255 for i in (1, 3, 5)]
    linear = [c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4 for c in channels]
    return 0.2126 * linear[0] + 0.7152 * linear[1] + 0.0722 * linear[2]


def contrast(a: str, b: str) -> float:
    high, low = sorted((luminance(a), luminance(b)), reverse=True)
    return (high + 0.05) / (low + 0.05)


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

    def test_rejects_colours_that_are_not_rrggbb(self):
        for bad in ("#FFF", "FF5A36", "#GG5A36", "#FF5A36AA", 16733750):
            tokens = copy.deepcopy(TOKENS)
            tokens["color"]["brand"]["light"] = bad
            with self.assertRaises(ValueError, msg=repr(bad)):
                tokens_to_qml.render(tokens)

    # DESIGN.md: text and status colours meet WCAG AA (4.5:1) on canvas, surface and raised in their theme.
    # brand is exempt: it is only the wordmark and the focus ring, a non-text component, so it needs 3:1 (WCAG 1.4.11).
    def test_text_and_status_colours_meet_aa(self):
        colors = TOKENS["color"]
        for mode in ("dark", "light"):
            for background in ("canvas", "surface", "raised"):
                for foreground in ("textPrimary", "textSecondary", "keep", "delete", "warning", "info"):
                    ratio = contrast(colors[foreground][mode], colors[background][mode])
                    self.assertGreaterEqual(ratio, 4.5, f"{foreground} on {background} ({mode}) is {ratio:.2f}:1")
                ratio = contrast(colors["brand"][mode], colors[background][mode])
                self.assertGreaterEqual(ratio, 3.0, f"brand on {background} ({mode}) is {ratio:.2f}:1")


if __name__ == "__main__":
    unittest.main()
