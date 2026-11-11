#!/usr/bin/env python3
"""Generate a small C lookup table from Emojibase's compact.json.

Usage: gen_aliases.py <compact.json> <out.c>

Emits one sorted entry per emoji: the emoji itself and its search text
(label + tags + emoticon, including skin tone variants).
"""
import json
import sys


def main():
    if len(sys.argv) != 3:
        print("usage: gen_aliases.py <compact.json> <out.c>", file=sys.stderr)
        return 2
    with open(sys.argv[1], encoding="utf-8") as f:
        data = json.load(f)

    entries = {}

    def add(emoji, parts):
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

    for item in data:
        parts = [item.get("label", "")] + list(item.get("tags") or [])
        if item.get("emoticon"):
            parts.append(item["emoticon"])
        add(item.get("unicode"), parts)
        for skin in item.get("skins") or []:
            add(skin.get("unicode"), parts)

    items = sorted(entries.items(), key=lambda kv: kv[0].encode("utf-8"))

    with open(sys.argv[2], "w", encoding="utf-8") as out:
        out.write("/* generated from emojibase-data en/compact.json; do not edit */\n")
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
    return 0


if __name__ == "__main__":
    sys.exit(main())
