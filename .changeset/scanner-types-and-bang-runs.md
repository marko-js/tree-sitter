---
"@marko/tree-sitter": patch
---

Match the rest of the htmljs-parser release: a `type`/`interface`/`declare` scriptlet (and a statement with extra spaces before the keyword) is read as a type, and `x!!`, `"s"!` and `` `s`! `` end an unenclosed value like `x!`.
