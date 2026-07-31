#!/usr/bin/env python3
"""Validate the repository's standalone monochrome architecture SVGs."""

from __future__ import annotations

import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SVG_DIRECTORY = ROOT / "docs" / "diagrams"
EXPECTED_FILES = (
    "concurrent-task-scheduling-engine-software-architecture.svg",
)
ALLOWED_COLORS = {
    "#000",
    "#000000",
    "black",
    "#fff",
    "#ffffff",
    "white",
    "none",
}
COLOR_PATTERN = re.compile(
    r"#[0-9a-fA-F]{3,8}\b|\b(?:rgb|rgba|hsl|hsla)\s*\([^)]*\)",
    re.IGNORECASE,
)
PROHIBITED_ELEMENTS = {"script", "foreignObject", "image"}
EXTERNAL_REFERENCE_PATTERN = re.compile(r"^(?:https?:|//|data:)", re.IGNORECASE)


def local_name(tag: str) -> str:
    return tag.rsplit("}", 1)[-1]


def validate_svg(path: Path) -> list[str]:
    errors: list[str] = []
    try:
        tree = ET.parse(path)
    except (ET.ParseError, OSError) as error:
        return [f"cannot parse XML: {error}"]

    root = tree.getroot()
    if local_name(root.tag) != "svg":
        errors.append("root element is not svg")
    if not root.get("viewBox"):
        errors.append("missing viewBox")
    if root.get("width") or root.get("height"):
        errors.append("root must scale through viewBox without fixed dimensions")

    children = list(root)
    titles = [element for element in children if local_name(element.tag) == "title"]
    descriptions = [
        element for element in children if local_name(element.tag) == "desc"
    ]
    if not titles or not "".join(titles[0].itertext()).strip():
        errors.append("missing non-empty title")
    if not descriptions or not "".join(descriptions[0].itertext()).strip():
        errors.append("missing non-empty desc")

    markers = 0
    for element in root.iter():
        name = local_name(element.tag)
        if name == "marker":
            markers += 1
        if name in PROHIBITED_ELEMENTS:
            errors.append(f"prohibited element: {name}")
        for attribute, value in element.attrib.items():
            attribute_name = local_name(attribute)
            normalized = value.strip().lower()
            if attribute_name in {"fill", "stroke", "color", "stop-color"}:
                if normalized not in ALLOWED_COLORS and not normalized.startswith("url(#"):
                    errors.append(
                        f"disallowed {attribute_name} value {value!r} on {name}"
                    )
            if attribute_name in {"opacity", "fill-opacity", "stroke-opacity"}:
                errors.append(f"opacity is prohibited on {name}")
            if attribute_name in {"href", "src"} and EXTERNAL_REFERENCE_PATTERN.match(
                normalized
            ):
                errors.append(f"external resource {value!r} on {name}")

    if markers == 0:
        errors.append("no reusable marker definition")

    source = path.read_text(encoding="utf-8")
    for color in COLOR_PATTERN.findall(source):
        if color.lower() not in ALLOWED_COLORS:
            errors.append(f"disallowed color token {color!r}")
    for token in ("<script", "<foreignObject", "<image", "javascript:"):
        if token.lower() in source.lower():
            errors.append(f"prohibited content token {token!r}")
    return sorted(set(errors))


def main() -> int:
    failures = 0
    for filename in EXPECTED_FILES:
        path = SVG_DIRECTORY / filename
        if not path.is_file():
            print(f"FAIL {path.relative_to(ROOT)}: missing")
            failures += 1
            continue
        errors = validate_svg(path)
        if errors:
            failures += 1
            print(f"FAIL {path.relative_to(ROOT)}")
            for error in errors:
                print(f"  - {error}")
        else:
            print(f"PASS {path.relative_to(ROOT)}")
    if failures:
        print(f"Architecture SVG validation failed: {failures} file(s)")
        return 1
    print(f"Architecture SVG validation passed: {len(EXPECTED_FILES)} file(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
