#ifndef CUIMOJI_EMOJIBASE_H
#define CUIMOJI_EMOJIBASE_H

/* Look up the Emojibase search text (label + tags + emoticon) for one emoji.
 * Returns NULL when the emoji is not in the data. */
const char *cuimoji_alias_lookup(const char *emoji, int len);

/* Emojibase shortcodes (for example "rofl"), space separated. NULL when the
 * emoji has none. Kept separate from the tags so search can rank an exact
 * shortcode above a tag. */
const char *cuimoji_shortcode_lookup(const char *emoji, int len);

#endif
