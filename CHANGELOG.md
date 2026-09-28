# @marko/tree-sitter

## 0.3.1

### Patch Changes

- [#14](https://github.com/marko-js/tree-sitter/pull/14) [`8438e07`](https://github.com/marko-js/tree-sitter/commit/8438e079838cabca78918b8cf970ce7dc1e48e9a) Thanks [@DylanPiercey](https://github.com/DylanPiercey)! - Match htmljs-parser 5.17: a `<!--` after a whitespace-terminated attribute value is an html comment rather than a less-than operator, so `<div class="a" <!-- note --> id="b">` reports an error instead of reading `"a" <!-- note -->` as the value.

- [`5c5ef68`](https://github.com/marko-js/tree-sitter/commit/5c5ef685a793d140365d21422ea510787117e6f0) Thanks [@DylanPiercey](https://github.com/DylanPiercey)! - Match htmljs-parser: a `!` directly after an operand (`x!`, `f()!`, `a[0]!`) is a TypeScript non-null assertion that ends an unenclosed value, `void` inside a type no longer continues onto what follows, and `delete` is read as a prefix operator.

- [#15](https://github.com/marko-js/tree-sitter/pull/15) [`9b57c34`](https://github.com/marko-js/tree-sitter/commit/9b57c34895849d323bd57595efdbd8c0068e505c) Thanks [@DylanPiercey](https://github.com/DylanPiercey)! - Match the rest of the htmljs-parser release: a `type`/`interface`/`declare` scriptlet (and a statement with extra spaces before the keyword) is read as a type, and `x!!`, `"s"!` and `` `s`! `` end an unenclosed value like `x!`.

## 0.3.0

### Minor Changes

- [#10](https://github.com/marko-js/tree-sitter/pull/10) [`86a40c2`](https://github.com/marko-js/tree-sitter/commit/86a40c288a466742a2e8e1f93620dc3eefce6079) Thanks [@DylanPiercey](https://github.com/DylanPiercey)! - Support the `async` shorthand method modifier as an `attr_method_async` node, eg `<button async onClick() { await save() }>`, keep a whitespace preceded `>=` in an attribute value instead of ending the tag, stop an unenclosed attribute value from swallowing a following close tag, and step over literals and comments when scanning an async method's signature.

## 0.2.0

### Minor Changes

- [#6](https://github.com/marko-js/tree-sitter/pull/6) [`d60112d`](https://github.com/marko-js/tree-sitter/commit/d60112d1bad21fc24fb8a62fec34063164f3ec15) Thanks [@DylanPiercey](https://github.com/DylanPiercey)! - Support comments between concise mode line attributes. `//` line and `/* */` block comments may now appear between comma-prefixed line attributes; they are scanned over and no longer terminate the open tag.
