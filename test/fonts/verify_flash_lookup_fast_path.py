from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[2]
FONT_HEADER = ROOT / "lib" / "EpdFont" / "SdFontFamily.h"
FONT_SOURCE = ROOT / "lib" / "EpdFont" / "SdFontFamily.cpp"
RENDERER_SOURCE = ROOT / "lib" / "GfxRenderer" / "GfxRenderer.cpp"


def main() -> int:
    header = FONT_HEADER.read_text(encoding="utf-8")
    source = FONT_SOURCE.read_text(encoding="utf-8")
    renderer = RENDERER_SOURCE.read_text(encoding="utf-8")

    checks = [
        ("lookup struct", "struct GlyphLookupResult" in header),
        ("lookup method declaration", "GlyphLookupResult lookupGlyph" in header),
        ("lookup method definition", "UnifiedFontFamily::GlyphLookupResult UnifiedFontFamily::lookupGlyph" in source),
        ("render receives unified lookup", "const UnifiedFontFamily::GlyphLookupResult& glyphLookup" in renderer),
        ("advance uses unified lookup", "font.lookupGlyph(cp, style)" in renderer),
    ]

    failed = [name for name, ok in checks if not ok]
    if failed:
        print("Missing fast lookup pieces: " + ", ".join(failed), file=sys.stderr)
        return 1

    print("OK: unified flash lookup fast path exists")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
