---
type: bug
impact: low
effort: med
site: src/scanner.c › scan_scriptlet_body
---

# Keep error recovery from reading an unclosed concise `$ {` body as tags

As in htmljs-parser 5.22, the body of a concise `$ {` block that EOF cuts off is an error, so `scan_scriptlet_body` refuses it. tree-sitter then recovers by inserting a MISSING `}` and asking `scan_concise` for content at the body's start, which reads the body as concise lines: `$ {${x}` becomes an element named by the placeholder, `$ {.a` one with a shorthand class. Editors show those junk elements while a block is typed at the end of a file, and their zero-width events fall inside the fixture comparison window. Record an open scriptlet block in the serialized state and refuse content tokens while it is open, so recovery wraps the rest in an ERROR instead.

Check: `pnpm exec tsx tools/check.mts '$ {${x}'` prints a tree with `(element (tag_name (placeholder …)))` after the scriptlet and reports `+ tagName.quasis[0] [3,3]`.
