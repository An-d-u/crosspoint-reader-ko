import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
BUILTIN_DIR = ROOT / "lib" / "EpdFont" / "builtinFonts"

FONT_DATA_RE = re.compile(
    r"static const EpdFontData\s+(\w+)\s*=\s*\{.*?^\s*(\d+),\s*$.*?^\s*(nullptr|\w+),\s*$",
    re.DOTALL | re.MULTILINE,
)
GROUP_BLOCK_RE = re.compile(r"static const EpdFontGroup\s+(\w+)\[\]\s*=\s*\{(.*?)\};", re.DOTALL)
GROUP_ENTRY_RE = re.compile(r"\{\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+)\s*\}")


def parse_group_blocks(text: str):
    groups = {}
    for match in GROUP_BLOCK_RE.finditer(text):
        name = match.group(1)
        entries = []
        for entry in GROUP_ENTRY_RE.finditer(match.group(2)):
            entries.append(
                {
                    "compressed_offset": int(entry.group(1)),
                    "compressed_size": int(entry.group(2)),
                    "uncompressed_size": int(entry.group(3)),
                    "glyph_count": int(entry.group(4)),
                    "first_glyph_index": int(entry.group(5)),
                }
            )
        groups[name] = entries
    return groups


def parse_font_data(text: str):
    result = []
    for match in FONT_DATA_RE.finditer(text):
        font_name = match.group(1)
        group_count = int(match.group(2))
        glyph_to_group = match.group(3)
        result.append((font_name, group_count, glyph_to_group))
    return result


def verify_header(path: Path):
    text = path.read_text(encoding="utf-8")
    group_blocks = parse_group_blocks(text)
    errors = []

    for font_name, group_count, glyph_to_group in parse_font_data(text):
        if group_count == 0 or glyph_to_group != "nullptr":
            continue

        groups_name = f"{font_name}Groups"
        groups = group_blocks.get(groups_name)
        if groups is None:
            errors.append(f"{path.name}: missing group block {groups_name}")
            continue

        if len(groups) != group_count:
            errors.append(f"{path.name}: expected {group_count} groups, found {len(groups)}")
            continue

        prev_end = None
        for index, group in enumerate(groups):
            first = group["first_glyph_index"]
            glyph_count = group["glyph_count"]
            if prev_end is not None and first < prev_end:
                errors.append(f"{path.name}: group {index} overlaps previous group ({first} < {prev_end})")
                break
            prev_end = first + glyph_count

    return errors


def main():
    errors = []
    for header in sorted(BUILTIN_DIR.glob("*.h")):
        errors.extend(verify_header(header))

    if errors:
        print("FAIL: contiguous compressed font groups are not binary-search safe.")
        for error in errors:
            print(f" - {error}")
        return 1

    print("PASS: contiguous compressed font groups are sorted and non-overlapping.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
