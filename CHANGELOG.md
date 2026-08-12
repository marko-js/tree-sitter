# @marko/tree-sitter

## 0.3.0

### Minor Changes

- [#10](https://github.com/marko-js/tree-sitter/pull/10) [`86a40c2`](https://github.com/marko-js/tree-sitter/commit/86a40c288a466742a2e8e1f93620dc3eefce6079) Thanks [@DylanPiercey](https://github.com/DylanPiercey)! - Support the `async` shorthand method modifier as an `attr_method_async` node, eg `<button async onClick() { await save() }>`, keep a whitespace preceded `>=` in an attribute value instead of ending the tag, stop an unenclosed attribute value from swallowing a following close tag, and step over literals and comments when scanning an async method's signature.

## 0.2.0

### Minor Changes

- [#6](https://github.com/marko-js/tree-sitter/pull/6) [`d60112d`](https://github.com/marko-js/tree-sitter/commit/d60112d1bad21fc24fb8a62fec34063164f3ec15) Thanks [@DylanPiercey](https://github.com/DylanPiercey)! - Support comments between concise mode line attributes. `//` line and `/* */` block comments may now appear between comma-prefixed line attributes; they are scanned over and no longer terminate the open tag.
