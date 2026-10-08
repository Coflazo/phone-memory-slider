#!/usr/bin/env python3
"""Generates desktop/qml/Theme.qml from design/tokens.json.

    python tools/tokens_to_qml.py            write Theme.qml
    python tools/tokens_to_qml.py --check    exit 1 if the committed Theme.qml differs (line endings ignored)

The output depends only on tokens.json (key order included), so two runs give identical bytes.
"""

import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TOKENS = ROOT / "design" / "tokens.json"
THEME = ROOT / "desktop" / "qml" / "Theme.qml"

# `delete` is a JavaScript keyword, so it cannot be a QML property name.
QML_NAMES = {"delete": "remove"}

# v1 screens still use these names; Plan C Task 11 deletes those screens and this table.
DEPRECATED = [
    ("bool", "darkMode", "dark"),
    ("color", "accent", "brand"),
    ("color", "cool", "info"),
    ("string", "displayFont", "fontSans"),
    ("string", "bodyFont", "fontSans"),
    ("string", "monoFont", "fontMono"),
    ("int", "controlRadius", "radiusControl"),
    ("int", "immediateDuration", "motionReduced"),
    ("int", "controlDuration", "motionChrome"),
    ("int", "expressiveDuration", "motionDeckSettle"),
]


def camel(prefix: str, key: str) -> str:
    return prefix + key[:1].upper() + key[1:]


def scalar(name: str, value) -> str:
    if isinstance(value, bool):
        raise ValueError(f"{name}: booleans are not tokens")
    if isinstance(value, int):
        return f"    readonly property int {name}: {value}"
    if isinstance(value, float):
        return f"    readonly property real {name}: {value!r}"
    if isinstance(value, str):
        return f"    readonly property string {name}: {json.dumps(value)}"
    raise ValueError(f"{name}: unsupported token value {value!r}")


def render(tokens: dict) -> str:
    lines = [
        "// generated from design/tokens.json, do not edit; run python tools/tokens_to_qml.py",
        "pragma Singleton",
        "import QtQuick",
        "",
        "QtObject {",
        "    readonly property bool dark: Qt.styleHints.colorScheme !== Qt.ColorScheme.Light",
        "",
    ]
    for name, modes in tokens["color"].items():
        lines.append(
            f'    readonly property color {QML_NAMES.get(name, name)}: dark ? "{modes["dark"]}" : "{modes["light"]}"'
        )
    lines.append("")
    for section, prefix in (("space", "space"), ("radius", "radius"), ("motionMs", "motion"), ("deck", "deck")):
        lines += [scalar(camel(prefix, key), value) for key, value in tokens[section].items()]
        lines.append("")
    typography = tokens["typography"]
    lines.append(scalar("fontSans", typography["sans"]))
    lines.append(scalar("fontMono", typography["mono"]))
    lines += [scalar(camel("fontSize", key), value) for key, value in typography["size"].items()]
    lines.append(scalar("fontWeightTitle", typography["titleWeight"]))
    lines.append(scalar("lineHeightBody", typography["bodyLineHeight"]))
    lines += ["", "    // Deprecated v1 names, removed in Plan C Task 11."]
    lines += [f"    readonly property {kind} {old}: {new}" for kind, old, new in DEPRECATED]
    lines += ["}", ""]
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true", help="compare instead of writing")
    parser.add_argument("--theme", type=Path, default=THEME, help="Theme.qml path (default: the committed one)")
    args = parser.parse_args()

    expected = render(json.loads(TOKENS.read_text(encoding="utf-8")))
    if args.check:
        actual = args.theme.read_bytes().decode("utf-8").replace("\r\n", "\n")
        if actual != expected:
            print(f"{args.theme} differs from design/tokens.json; run python tools/tokens_to_qml.py", file=sys.stderr)
            return 1
        return 0
    args.theme.write_bytes(expected.encode("utf-8"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
