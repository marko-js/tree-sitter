---
"@marko/tree-sitter": minor
---

Support the `async` shorthand method modifier as an `attr_method_async` node, eg `<button async onClick() { await save() }>`, keep a whitespace preceded `>=` in an attribute value instead of ending the tag, stop an unenclosed attribute value from swallowing a following close tag, and step over literals and comments when scanning an async method's signature.
