# @marko/tree-sitter

## 0.5.0

### Minor Changes

- [#22](https://github.com/marko-js/tree-sitter/pull/22) [`d10d4bd`](https://github.com/marko-js/tree-sitter/commit/d10d4bdd147875ddadef0a52f2b5dc3a1bd263cc) Thanks [@DylanPiercey](https://github.com/DylanPiercey)! - Match htmljs-parser 5.21 and 5.22: an html comment ends at the first `-->` (so `<!-- a->b -->` is one comment), a `/` after a keyword an operand follows (`return /x/`, `typeof /x/`) starts a regular expression, and in a `<script>` or `<style>` body a backslash-escaped quote, backtick or slash is text, a regular expression literal is skipped whole, and any escape in a string is text. A spaced non-null `!` (`a=x ! b`) ends an unenclosed value while `return !x` keeps a prefix `!`, a value continues past leading comments (`x=/* note */ 1`), an html mode attribute name cut off by EOF (`<div foo`) is still an `attr_name`, and in concise mode an unclosed `(…`, `{…`, `<…`, `|…`, `$ {…` or `${…` at EOF is an error.

### Patch Changes

- [#24](https://github.com/marko-js/tree-sitter/pull/24) [`2b73897`](https://github.com/marko-js/tree-sitter/commit/2b73897707942d0dd25b17f26e8230dea2546f33) Thanks [@DylanPiercey](https://github.com/DylanPiercey)! - Match htmljs-parser: an attribute value holding only a comment ends at the comment rather than taking in the whitespace before a `>` that ends the tag (`<div a=/* c */ >`).

- [#25](https://github.com/marko-js/tree-sitter/pull/25) [`664b1b1`](https://github.com/marko-js/tree-sitter/commit/664b1b1fc707445c5e5cdcd9ac3707785b7ff775) Thanks [@DylanPiercey](https://github.com/DylanPiercey)! - Parse a `<textarea>` body as text (with placeholders), as htmljs-parser and the Marko compiler do, rather than as html.

## 0.4.1

### Patch Changes

- [#20](https://github.com/marko-js/tree-sitter/pull/20) [`4180ef8`](https://github.com/marko-js/tree-sitter/commit/4180ef8a8a22fbc23b38162a9751eda1c4bd1215) Thanks [@DylanPiercey](https://github.com/DylanPiercey)! - Highlight the `async` modifier of a shorthand method (`attr_method_async`) as a keyword. Since the node was added it had no capture, so `async` lost the attribute highlighting it had before.

## 0.4.0

### Minor Changes

- [#18](https://github.com/marko-js/tree-sitter/pull/18) [`e52f872`](https://github.com/marko-js/tree-sitter/commit/e52f87273a244749f34e471b56bb183c11c1fc24) Thanks [@DylanPiercey](https://github.com/DylanPiercey)! - Match htmljs-parser 5.20's concise line starts: a line starting with `-` and a word character (eg `-if(x)`, `-else-if`, `-my-tag`) opens a tag whose name starts with `-`, and a line starting with `;` is ignored, with only comments allowed after it. An open tag that ends before its name (eg a line of just `,`, or `<,>`) now gets an empty `tag_name` instead of failing to parse, and a name after a leading `,` and comments (eg `, /* note */ div`) is read as the tag name.

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
