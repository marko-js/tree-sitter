---
"@marko/tree-sitter": minor
---

Match htmljs-parser 5.21 and 5.22: an html comment ends at the first `-->` (so `<!-- a->b -->` is one comment), a `/` after a keyword an operand follows (`return /x/`, `typeof /x/`) starts a regular expression, and in a `<script>` or `<style>` body a backslash-escaped quote, backtick or slash is text, a regular expression literal is skipped whole, and any escape in a string is text. A spaced non-null `!` (`a=x ! b`) ends an unenclosed value while `return !x` keeps a prefix `!`, a value continues past leading comments (`x=/* note */ 1`), an html mode attribute name cut off by EOF (`<div foo`) is still an `attr_name`, and in concise mode an unclosed `(…`, `{…`, `<…`, `|…`, `$ {…` or `${…` at EOF is an error.
