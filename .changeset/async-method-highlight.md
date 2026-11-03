---
"@marko/tree-sitter": patch
---

Highlight the `async` modifier of a shorthand method (`attr_method_async`) as a keyword. Since the node was added it had no capture, so `async` lost the attribute highlighting it had before.
