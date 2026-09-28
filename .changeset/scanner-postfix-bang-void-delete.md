---
"@marko/tree-sitter": patch
---

Match htmljs-parser: a `!` directly after an operand (`x!`, `f()!`, `a[0]!`) is a TypeScript non-null assertion that ends an unenclosed value, `void` inside a type no longer continues onto what follows, and `delete` is read as a prefix operator.
