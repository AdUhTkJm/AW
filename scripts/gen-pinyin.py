#!/usr/bin/env python3
"""Generate Pinyin.h from the mozillazg/pinyin-data dump.

CMake runs this at build time; the header is an artifact and is never committed.
The input is third_party/pinyin.txt (see scripts/fetch-pinyin.sh), whose lines
look like:

    U+91CD: zhòng,chóng,tóng  # 重

Only the CJK Unified Ideographs block U+4E00..U+9FA5 is emitted. That is the
block the item names actually use and it is inclusive of the pre-Unicode 1.1
upper bound U+9FA5, which keeps the lookup a direct table instead of a search.

Tones are dropped and the result is ASCII a-z:

  * readings are decomposed with NFD, so every tone is a combining mark;
  * a diaeresis (U+0308) becomes 'v' in one variant and is dropped in a second
    one, so both "lv" and "lu" spellings match 女/绿/略;
  * every remaining combining mark is removed.

The generated header has three tables:

  * kStringPool: the distinct toneless readings back to back, each one
    '\\0'-terminated, so a reading is just `kStringPool + offset`;
  * kExtra: the additional readings of a character, as string-pool offsets,
    each list terminated by 0xFFFF;
  * kEntries: one PinyinEntry per codepoint in the block, indexed by
    `codepoint - kFirstCodepoint`. A character with no reading under the block
    title is marked by offset == 0xFFFF.

`--self-test` parses the input and checks a few well-known readings without
emitting anything, which is what the CTest case runs.
"""

import argparse
import re
import sys
import unicodedata
from pathlib import Path

# The emitted block. U+9FA5 is kept even though Unicode now extends the block to
# U+9FFF: it is the classic GBK boundary and matches the requested range.
CJK_FIRST = 0x4E00
CJK_LAST = 0x9FA5

# Slot marker for "no string-pool offset" and for the end of an extra list. The
# pool is far shorter than 64 KiB, so a real offset can never collide with it.
NO_VALUE = 0xFFFF

# One entry per line, sixteen values per line for the flat arrays.
ENTRIES_PER_LINE = 16
VALUES_PER_LINE = 16

LINE_RE = re.compile(r"^U\+([0-9A-Fa-f]+)\s*:\s*([^#]*?)\s*(?:#.*)?$")
VERSION_RE = re.compile(r"#\s*version:\s*(\S+)")


def toneless_forms(reading):
    """Return the ASCII spellings of one reading, diaeresis variants included."""
    decomposed = unicodedata.normalize("NFD", reading)
    # Map the diaeresis to 'v' before dropping the other combining marks. Doing
    # it the other way around would fold 女 nǚ and 努 nǔ onto the same "nu".
    # NFD decomposes ǚ into u, U+0308 and the tone mark, so the u goes too: the
    # pair becomes 'v', not 'uv'.
    with_v = decomposed.replace("u\u0308", "v")
    with_v = "".join(ch for ch in with_v if not unicodedata.combining(ch))
    forms = [with_v]
    if "\u0308" in decomposed:
        # The u-spelling is the fallback most IMEs also accept.
        forms.append("".join(ch for ch in decomposed if not unicodedata.combining(ch)))
    return forms


def first_letter_mask(forms):
    mask = 0
    for form in forms:
        mask |= 1 << (ord(form[0]) - ord("a"))
    return mask


def parse(path):
    """Parse the dump into {codepoint: [toneless reading, ...]}."""
    entries = {}
    version = None
    for lineno, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if not line:
            continue
        if line.startswith("#"):
            match = VERSION_RE.match(line)
            if match and version is None:
                version = match.group(1)
            continue

        match = LINE_RE.match(line)
        if match is None:
            raise SystemExit(f"{path}:{lineno}: unrecognised line: {line!r}")

        codepoint = int(match.group(1), 16)
        if not CJK_FIRST <= codepoint <= CJK_LAST:
            continue

        forms = []
        for reading in match.group(2).split(","):
            reading = reading.strip()
            if not reading:
                continue
            for form in toneless_forms(reading):
                if form not in forms:
                    forms.append(form)

        for form in forms:
            if not form or any(ch < "a" or ch > "z" for ch in form):
                raise SystemExit(
                    f"{path}:{lineno}: reading of U+{codepoint:04X} is not ASCII: {form!r}"
                )

        if codepoint in entries:
            raise SystemExit(f"{path}:{lineno}: U+{codepoint:04X} appears twice")
        entries[codepoint] = forms
    return version, entries


def build(entries):
    """Lay the readings out into the pool, the entry table and the extra array."""
    syllables = sorted({form for forms in entries.values() for form in forms})

    offset = {}
    cursor = 0
    for syllable in syllables:
        offset[syllable] = cursor
        cursor += len(syllable) + 1

    # kEntries is indexed by codepoint offset, so the holes inside the block are
    # materialised as explicit empty records rather than skipped.
    records = []
    extras = []
    for codepoint in range(CJK_FIRST, CJK_LAST + 1):
        forms = entries.get(codepoint, [])
        if not forms:
            records.append((0, NO_VALUE, NO_VALUE))
            continue
        extra_index = NO_VALUE
        if len(forms) > 1:
            extra_index = len(extras)
            extras.extend(offset[form] for form in forms[1:])
            extras.append(NO_VALUE)
        records.append((first_letter_mask(forms), offset[forms[0]], extra_index))
    return syllables, offset, records, extras


def c_string(chunks):
    """Render the pool as concatenated C string literals, one per line."""
    lines = []
    for start in range(0, len(chunks), ENTRIES_PER_LINE):
        literal = "".join(syllable + "\\0" for syllable in chunks[start:start + ENTRIES_PER_LINE])
        lines.append(f'    "{literal}"')
    return "\n".join(lines)


def emit(version, syllables, offset, records, extras, source_name):
    count = CJK_LAST - CJK_FIRST + 1
    pool_size = sum(len(syllable) + 1 for syllable in syllables)
    out = []
    out.append("// Generated by scripts/gen-pinyin.py from "
               f"{source_name}. DO NOT EDIT.")
    out.append("//")
    out.append("// Pinyin lookup for the item search index. One entry per codepoint in")
    out.append(f"// U+{CJK_FIRST:04X}..U+{CJK_LAST:04X}, tones dropped, ASCII a-z only. See the script for")
    out.append("// the encoding and the generator's --self-test for the expected readings.")
    if version:
        out.append(f"// pinyin-data version: {version}")
    out.append("")
    out.append("#ifndef AW_SEARCH_PINYIN_H")
    out.append("#define AW_SEARCH_PINYIN_H")
    out.append("")
    out.append("#include <cstdint>")
    out.append("#include <string_view>")
    out.append("")
    out.append("namespace aw::search::pinyin {")
    out.append("")
    out.append(f"inline constexpr uint32_t kFirstCodepoint = 0x{CJK_FIRST:04X}u;")
    out.append(f"inline constexpr uint32_t kLastCodepoint = 0x{CJK_LAST:04X}u;")
    out.append(f"inline constexpr uint32_t kEntryCount = {count}u;")
    out.append(f"inline constexpr uint32_t kPoolSize = {pool_size}u;")
    out.append(f"inline constexpr uint32_t kExtraCount = {len(extras)}u;")
    out.append("")
    out.append("// Marks a missing string-pool offset (a character with no reading) and the")
    out.append("// end of an extra list. The pool is tiny, so it never collides with one.")
    out.append(f"inline constexpr uint16_t kNoValue = 0x{NO_VALUE:04X}u;")
    out.append("")
    out.append("// `firstLetter` is a bit mask of the readings' initials: bit (c - 'a') is set")
    out.append("// when some reading of the character starts with c. 重, read zhong/chong/tong,")
    out.append("// has the bits for 'c', 't' and 'z' set.")
    out.append("// `offset` is the primary reading's offset into kStringPool, or kNoValue.")
    out.append("// `extra` indexes the first additional reading in kExtra, or kNoValue.")
    out.append("struct PinyinEntry {")
    out.append("  uint32_t firstLetter;")
    out.append("  uint16_t offset;")
    out.append("  uint16_t extra;")
    out.append("};")
    out.append("")
    out.append("// The distinct toneless readings, NUL-separated and NUL-terminated, so")
    out.append("// std::string_view(kStringPool + offset) is a complete reading.")
    out.append("inline constexpr char kStringPool[] =")
    out.append(c_string(syllables) + ";")
    out.append("")
    out.append("// Additional readings as string-pool offsets; each list ends at kNoValue.")
    out.append("inline constexpr uint16_t kExtra[] = {")
    for start in range(0, len(extras), VALUES_PER_LINE):
        values = ", ".join(f"0x{value:04X}u" for value in extras[start:start + VALUES_PER_LINE])
        out.append(f"  {values},")
    out.append("};")
    out.append("")
    out.append("// Indexed by codepoint - kFirstCodepoint. Holes in the block are all-zero")
    out.append("// records with offset == kNoValue, so a lookup never needs a search.")
    by_offset = {value: name for name, value in offset.items()}
    out.append("inline constexpr PinyinEntry kEntries[] = {")
    for codepoint in range(CJK_FIRST, CJK_LAST + 1):
        mask, primary, extra = records[codepoint - CJK_FIRST]
        character = chr(codepoint)
        if primary == NO_VALUE:
            comment = f"U+{codepoint:04X} {character}"
        else:
            readings = reading_names(primary, extra, by_offset, extras)
            comment = f"U+{codepoint:04X} {character} {','.join(readings)}"
        out.append(f"  {{0x{mask:08X}u, 0x{primary:04X}u, 0x{extra:04X}u}},  // {comment}")
    out.append("};")
    out.append("")
    out.append("static_assert(kEntryCount == kLastCodepoint - kFirstCodepoint + 1u);")
    out.append("static_assert(sizeof(kStringPool) - 1 == kPoolSize,")
    out.append('              "the string pool and kPoolSize disagree");')
    out.append("static_assert(kExtraCount < kNoValue,"
               ' "an extra index must not collide with kNoValue");')
    out.append("static_assert(kPoolSize < kNoValue,"
               ' "a pool offset must not collide with kNoValue");')
    out.append("")
    out.append("// The entry for a codepoint, or nullptr when it is outside the block or has")
    out.append("// no reading. When non-null, the primary reading is always present.")
    out.append("[[nodiscard]] inline const PinyinEntry *find(uint32_t codepoint) noexcept {")
    out.append("  if (codepoint < kFirstCodepoint || codepoint > kLastCodepoint)")
    out.append("    return nullptr;")
    out.append("  const PinyinEntry &entry = kEntries[codepoint - kFirstCodepoint];")
    out.append("  return entry.offset == kNoValue ? nullptr : &entry;")
    out.append("}")
    out.append("")
    out.append("// Calls `fn(std::string_view)` for the primary reading and every extra one.")
    out.append("template <typename Fn>")
    out.append("inline void forEachReading(const PinyinEntry &entry, Fn &&fn) {")
    out.append("  if (entry.offset == kNoValue)")
    out.append("    return;")
    out.append("  fn(std::string_view(kStringPool + entry.offset));")
    out.append("  for (uint32_t i = entry.extra; i != kNoValue; i++) {")
    out.append("    const uint16_t reading = kExtra[i];")
    out.append("    if (reading == kNoValue)")
    out.append("      break;")
    out.append("    fn(std::string_view(kStringPool + reading));")
    out.append("  }")
    out.append("}")
    out.append("")
    out.append("}  // namespace aw::search::pinyin")
    out.append("")
    out.append("#endif  // AW_SEARCH_PINYIN_H")
    out.append("")
    return "\n".join(out)


def reading_names(primary, extra, by_offset, extras):
    """Recover the reading list of one character for the entry comment."""
    names = [by_offset[primary]]
    index = extra
    while index != NO_VALUE:
        reading = extras[index]
        if reading == NO_VALUE:
            break
        names.append(by_offset[reading])
        index += 1
    return names


def self_test(source, entries):
    expected = {
        0x91CD: {"zhong", "chong", "tong"},   # 重
        0x4E2D: {"zhong"},                    # 中, first reading wins as primary
        0x5973: {"nv", "nu", "ru"},           # 女, both diaeresis spellings
        0x7565: {"lve", "lue"},               # 略
        0x957F: {"zhang", "chang"},           # 长
        0x897F: {"xi"},                       # 西
        0x5159: set(),                        # 兙, a hole in the block
    }
    failures = []
    for codepoint, wanted in expected.items():
        got = set(entries.get(codepoint, []))
        if got != wanted:
            failures.append(f"U+{codepoint:04X}: expected {sorted(wanted)}, got {sorted(got)}")

    # The primary reading must be the first one in the source, because that is
    # what the ordering of the dump encodes.
    if entries.get(0x4E2D, [None])[0] != "zhong":
        failures.append("U+4E2D: primary reading is not zhong")

    if failures:
        for failure in failures:
            print(f"self-test: {failure}", file=sys.stderr)
        return 1
    print(f"self-test: {len(entries)} characters checked out of {CJK_LAST - CJK_FIRST + 1} slots")
    return 0


def main(argv=None):
    root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("input", nargs="?", default=str(root / "third_party" / "pinyin.txt"),
                        help="pinyin.txt to read (default: third_party/pinyin.txt)")
    parser.add_argument("output", nargs="?",
                        help="header to write (default: standard output)")
    parser.add_argument("--self-test", action="store_true",
                        help="check known readings instead of generating")
    args = parser.parse_args(argv)

    source = Path(args.input)
    version, entries = parse(source)

    if args.self_test:
        return self_test(source, entries)

    syllables, offset, records, extras = build(entries)
    text = emit(version, syllables, offset, records, extras, source.name)

    if args.output:
        Path(args.output).write_text(text, encoding="utf-8")
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
