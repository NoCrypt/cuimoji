#!/usr/bin/env python3
"""Generate the Emojibase search tables.

Usage: gen_aliases.py <compact.json> <shortcodes.json> <out.c>

compact.json gives each emoji's label, tags and emoticon. shortcodes.json is
Emojibase's shortcode map (en/shortcodes/emojibase.json); it is kept separate
so the search can rank an exact shortcode like "rofl" above a mere tag.
"""
import json
import os
import sys


def main():
    if len(sys.argv) != 4:
        print("usage: gen_aliases.py <compact.json> <shortcodes.json> <out.c>",
              file=sys.stderr)
        return 2
    with open(sys.argv[1], encoding="utf-8") as f:
        data = json.load(f)

    shortcodes = {}
    if os.path.exists(sys.argv[2]):
        with open(sys.argv[2], encoding="utf-8") as f:
            shortcodes = json.load(f)

    entries = {}
    short_entries = {}

    def add(emoji, parts, codes):
        if not emoji:
            return
        words = []
        for p in parts:
            if isinstance(p, list):
                words.extend(str(x) for x in p)
            elif p:
                words.append(str(p))
        text = " ".join(words)
        old = entries.get(emoji)
        if old is None or len(text) > len(old):
            entries[emoji] = text
        if codes:
            oldc = short_entries.get(emoji)
            if oldc is None or len(codes) > len(oldc):
                short_entries[emoji] = codes

    for item in data:
        parts = [item.get("label", "")] + list(item.get("tags") or [])
        if item.get("emoticon"):
            parts.append(item["emoticon"])
        code = shortcodes.get(item.get("hexcode", ""), "")
        if isinstance(code, list):
            code = " ".join(str(x) for x in code)
        elif not code:
            code = ""
        add(item.get("unicode"), parts, code)
        for skin in item.get("skins") or []:
            add(skin.get("unicode"), parts, code)

    items = sorted(entries.items(), key=lambda kv: kv[0].encode("utf-8"))
    shorts = sorted(short_entries.items(), key=lambda kv: kv[0].encode("utf-8"))

    with open(sys.argv[3], "w", encoding="utf-8") as out:
        out.write("/* generated from emojibase-data; do not edit */\n")
        out.write('#include "emojibase.h"\n')
        out.write("#include <string.h>\n\n")
        out.write("typedef struct { const char *emoji; const char *aliases; } cui__alias_entry;\n\n")
        out.write("static const cui__alias_entry cui__aliases[] = {\n")
        for emoji, text in items:
            out.write("\t{ %s, %s },\n" % (json.dumps(emoji, ensure_ascii=False),
                                          json.dumps(text, ensure_ascii=False)))
        out.write("};\n\n")
        out.write("""const char *cuimoji_alias_lookup(const char *emoji, int len) {
    int lo = 0, hi = (int)(sizeof cui__aliases / sizeof cui__aliases[0]) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        const char *e = cui__aliases[mid].emoji;
        int c = strncmp(emoji, e, (size_t)len);
        if (c == 0 && e[len] != 0) c = -1;
        if (c == 0) return cui__aliases[mid].aliases;
        if (c < 0) hi = mid - 1;
        else lo = mid + 1;
    }
    return NULL;
}

""")
        out.write("typedef struct { const char *emoji; const char *shortcodes; } cui__shortcode_entry;\n\n")
        if shorts:
            out.write("static const cui__shortcode_entry cui__shortcodes[] = {\n")
            for emoji, codes in shorts:
                out.write("\t{ %s, %s },\n" % (json.dumps(emoji, ensure_ascii=False),
                                              json.dumps(codes, ensure_ascii=False)))
            out.write("};\n\n#define CUI__SHORTCODE_COUNT %d\n\n" % len(shorts))
        else:
            out.write('static const cui__shortcode_entry cui__shortcodes[] = { { "", "" } };\n\n'
                      '#define CUI__SHORTCODE_COUNT 0\n\n')
        out.write("""const char *cuimoji_shortcode_lookup(const char *emoji, int len) {
    if (!CUI__SHORTCODE_COUNT) return NULL;
    int lo = 0, hi = CUI__SHORTCODE_COUNT - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        const char *e = cui__shortcodes[mid].emoji;
        int c = strncmp(emoji, e, (size_t)len);
        if (c == 0 && e[len] != 0) c = -1;
        if (c == 0) return cui__shortcodes[mid].shortcodes;
        if (c < 0) hi = mid - 1;
        else lo = mid + 1;
    }
    return NULL;
}
""")
    return 0


if __name__ == "__main__":
    sys.exit(main())
