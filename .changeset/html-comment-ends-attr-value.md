---
"@marko/tree-sitter": patch
---

Match htmljs-parser 5.17: a `<!--` after a whitespace-terminated attribute value is an html comment rather than a less-than operator, so `<div class="a" <!-- note --> id="b">` reports an error instead of reading `"a" <!-- note -->` as the value.
