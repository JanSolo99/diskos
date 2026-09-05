#!/usr/bin/env python3
"""Fail on a UI string literal the device cannot draw.

The fonts diskOS ships cover far less than a desktop developer's editor does:

  Montserrat (th_font)          U+0020..U+007E only, plus the LV_SYMBOL private-use
                                icons at U+F000+
  Source Han SC (ui_font_cjk)   U+3001 and up - and ONLY on the labels that opt into
                                the CJK fallback
  font_icons_28 / weather16     private-use FontAwesome glyphs

Everything in between - the typographic punctuation an editor inserts without asking,
and every accented Latin letter - has no glyph in any font we load, so LVGL draws
LV_USE_FONT_PLACEHOLDER: a black box. Tag text is folded at runtime by ui/txtfold.c,
but a literal baked into our own source is not, and each one is a permanent box on
screen. "Turning on Wi-Fi<box>" shipped that way.

So: no undrawable codepoints in string literals. Use "..." not U+2026, "-" not an en
or em dash, LV_SYMBOL_OK not a check mark. Comments are ignored - they are not drawn.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
UI = os.path.join(ROOT, "ui")

SKIP_DIRS = {"lvgl", "tests"}
SKIP_FILES = {"sqlite3.c", "sqlite3.h", "jsmn.h"}

# What a font we load can actually draw.
def drawable(cp):
    if 0x20 <= cp <= 0x7E:      return True   # Montserrat
    if cp in (0x09, 0x0A, 0x0D): return True  # tab / newline / CR
    if 0xE000 <= cp <= 0xF8FF:  return True   # LV_SYMBOL + FontAwesome private use
    if cp >= 0x3001:            return True   # Source Han CJK fallback
    return False


def strip_comments(src):
    """Blank out // and /* */ comments, preserving offsets and newlines."""
    out = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c == '"' or c == "'":                      # a string/char literal: copy verbatim
            q = c
            out.append(c); i += 1
            while i < n:
                if src[i] == "\\" and i + 1 < n:
                    out.append(src[i]); out.append(src[i+1]); i += 2; continue
                out.append(src[i])
                if src[i] == q:
                    i += 1; break
                if src[i] == "\n":                    # unterminated: bail out of the literal
                    i += 1; break
                i += 1
            continue
        if src.startswith("//", i):
            while i < n and src[i] != "\n":
                out.append(" "); i += 1
            continue
        if src.startswith("/*", i):
            while i < n and not src.startswith("*/", i):
                out.append("\n" if src[i] == "\n" else " "); i += 1
            out.append("  "); i += 2
            continue
        out.append(c); i += 1
    return "".join(out)


# Host-only self-test blocks (#ifdef LFM_SELFTEST, #ifdef SCANNER_TEST ...) never
# reach the device, so their fixtures are free to contain anything.
TESTGUARD = re.compile(r'^\s*#\s*if(?:n?def)?\s+.*\b\w*(?:SELFTEST|_TEST)\b')
COND      = re.compile(r'^\s*#\s*(if|ifdef|ifndef|endif)\b')


def strip_host_test_blocks(src):
    """Blank out #ifdef *_TEST / *_SELFTEST regions, keeping line numbers intact."""
    out, depth = [], None
    for line in src.split("\n"):
        m = COND.match(line)
        if depth is None:
            if TESTGUARD.match(line):
                depth = 1
                out.append("")
                continue
            out.append(line)
        else:
            if m:
                if m.group(1) == "endif":
                    depth -= 1
                    if depth == 0:
                        depth = None
                else:
                    depth += 1
            out.append("")
    return "\n".join(out)


STRING = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
ESCAPE = re.compile(r'\\x([0-9a-fA-F]{2})|\\([0-7]{1,3})')


def literal_bytes(lit):
    """Decode a C string literal's escapes into the bytes it will actually hold."""
    out = bytearray()
    i, n = 0, len(lit)
    while i < n:
        if lit[i] == "\\" and i + 1 < n:
            m = ESCAPE.match(lit, i)
            if m:
                out.append(int(m.group(1), 16) if m.group(1) else int(m.group(2), 8))
                i = m.end(); continue
            out.append(ord(lit[i+1]) if ord(lit[i+1]) < 128 else 0x3F)
            i += 2; continue
        out.extend(lit[i].encode("utf-8"))
        i += 1
    return bytes(out)


def scan_file(path):
    raw = open(path, "rb").read()
    try:
        src = raw.decode("utf-8")
    except UnicodeDecodeError:
        src = raw.decode("latin-1")
    code = strip_comments(strip_host_test_blocks(src))
    bad = []
    for m in STRING.finditer(code):
        data = literal_bytes(m.group(1))
        try:
            text = data.decode("utf-8")
        except UnicodeDecodeError:
            continue                                  # binary blob / magic bytes, not display text
        # A literal carrying NUL or a control byte is DATA (a file magic, a wire
        # frame), never something we draw - a file magic is not a display string.
        if any(ord(c) < 0x09 or 0x0E <= ord(c) < 0x20 for c in text):
            continue
        for ch in text:
            if not drawable(ord(ch)):
                line = code.count("\n", 0, m.start()) + 1
                bad.append((line, ord(ch), m.group(0)[:70]))
                break
    return bad


def main():
    findings = []
    for root, dirs, files in os.walk(UI):
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
        for fn in sorted(files):
            if not fn.endswith((".c", ".h")) or fn in SKIP_FILES or fn.startswith("font_"):
                continue
            path = os.path.join(root, fn)
            for line, cp, snippet in scan_file(path):
                findings.append((os.path.relpath(path, ROOT), line, cp, snippet))

    print("-- undrawable literals -----------------------------------------------")
    for rel, line, cp, snippet in findings:
        print(f"  {rel}:{line}: U+{cp:04X} in {snippet}")
    if findings:
        print(f"\n{len(findings)} CHECK(S) FAILED")
        print("  no font diskOS loads has these glyphs - they draw as a black box.")
        print("  use ASCII (\"...\", \"-\") or an LV_SYMBOL_* icon instead.")
        return 1
    print("  none - every UI string literal is drawable")
    print("\nALL CHECKS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
