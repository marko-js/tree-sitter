---
"@marko/tree-sitter": patch
---

Match htmljs-parser: an attribute value holding only a comment ends at the comment rather than taking in the whitespace before a `>` that ends the tag (`<div a=/* c */ >`).
