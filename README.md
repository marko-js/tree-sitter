# tree-sitter-marko

A [tree-sitter](https://tree-sitter.github.io/) grammar for the
[Marko](https://markojs.com/) templating language.

It covers the full language: both authoring modes (HTML and
concise/indentation-based), `${placeholders}`, tag variables, arguments and
parameters, shorthand `#id`/`.class` and method attributes, attribute
groups, statement tags, raw-text tags, `--` content blocks, and `$`
scriptlets — all with the exact source ranges the Marko compiler sees, so
the tree is reliable for highlighting, folding, structural editing, and
tooling.

## Usage

With the node bindings:

```js
const Parser = require("tree-sitter");
const Marko = require("tree-sitter-marko");

const parser = new Parser();
parser.setLanguage(Marko);

const tree = parser.parse("<button onClick() { count++ }>${count}</button>");
console.log(tree.rootNode.toString());
```

In the browser (or anywhere native bindings are unwanted), use the wasm
build with [web-tree-sitter](https://github.com/tree-sitter/tree-sitter/tree/master/lib/binding_web):

```js
import { Language, Parser } from "web-tree-sitter";

await Parser.init();
const Marko = await Language.load("tree-sitter-marko.wasm");
const parser = new Parser();
parser.setLanguage(Marko);
```

Building from a checkout:

```sh
npm install
npm run build        # tree-sitter generate && node-gyp rebuild
npm run build:wasm   # tree-sitter-marko.wasm (the CLI fetches wasi-sdk itself)
```

For editors, the grammar ships ready-to-use queries:

- `queries/highlights.scm` — syntax highlighting captures
- `queries/injections.scm` — embedded-language injections (see below)

## Editors and tools

Every integration consumes the same artifacts: the generated parser
(`src/parser.c` + `src/scanner.c`), the queries in `queries/`, and — for
embedded highlighting — the `typescript`, `css`, and `scss` grammars,
installed in the host tool like any other language.

### tree-sitter CLI

From this directory:

```sh
npx tree-sitter playground            # interactive tree in the browser
npx tree-sitter parse file.marko      # print the syntax tree
npx tree-sitter highlight file.marko  # ANSI highlighting (--html for a page)
```

`highlight` resolves the injected languages through the CLI config: run
`tree-sitter init-config`, then make sure one of the `parser-directories`
in `~/.config/tree-sitter/config.json` holds clones of
`tree-sitter-typescript` (run `npm install --ignore-scripts` inside it —
its queries reference its `tree-sitter-javascript` dependency),
`tree-sitter-css`, and `tree-sitter-scss`. Note the loader only discovers
grammars that have a `tree-sitter.json`.

### Neovim

Neovim (0.10+) needs no plugins — compile the parser onto the runtime path
and start treesitter for the filetype:

```sh
# from this directory
mkdir -p ~/.local/share/nvim/site/parser ~/.local/share/nvim/site/queries
npx tree-sitter build -o ~/.local/share/nvim/site/parser/marko.so
cp -R queries ~/.local/share/nvim/site/queries/marko
```

```lua
-- init.lua
vim.filetype.add({ extension = { marko = "marko" } })
vim.api.nvim_create_autocmd("FileType", {
  pattern = "marko",
  callback = function()
    vim.treesitter.start()
  end,
})
```

Embedded highlighting uses whatever parsers are installed; with
nvim-treesitter that's `:TSInstall typescript css scss`.

### Helix

Register the grammar and language in `~/.config/helix/languages.toml`:

```toml
[[language]]
name = "marko"
scope = "source.marko"
file-types = ["marko"]
comment-token = "//"
block-comment-tokens = { start = "<!--", end = "-->" }
injection-regex = "^marko$"

[[grammar]]
name = "marko"
source = { git = "https://github.com/marko-js/tree-sitter", rev = "<commit>" }
```

(or `source = { path = "/path/to/tree-sitter-marko" }` for a local
checkout), then:

```sh
hx --grammar fetch && hx --grammar build
```

and copy the queries to `~/.config/helix/runtime/queries/marko/`.

### Zed

Marko support in Zed is provided by the
[marko-js/zed](https://github.com/marko-js/zed) extension, which
bundles this grammar (via its `[grammars.marko]` entry pointing here), the
language config, the editor queries, and the
[Marko language server](https://github.com/marko-js/language-server). Install
it from Zed's extension registry, or run **`zed: install dev extension`** on a
local checkout of that repo. Its `highlights.scm`/`injections.scm` are copies
of the `queries/` here (used as-is — Zed evaluates the
`#eq?`/`#any-of?`/`#not-any-of?` predicates, `injection.combined`, and the
dynamic `@injection.language` dialects), and the Zed-specific `brackets.scm`
and `outline.scm` are maintained in that extension repo.

### VS Code

VS Code does not consume tree-sitter grammars. Marko support there remains
the tmLanguage-based
[Marko VS Code extension](https://github.com/marko-js/language-server),
which this grammar's queries are designed to match visually.

## The tree

```marko
div.panel
  <button onClick() { count++ }>${count}</button>
```

```
(document
  (element
    (tag_name (tag_name_fragment))
    (shorthand_class (tag_name_fragment))
    (concise_open_tag_end)
    (element
      (open_tag_start)
      (tag_name (tag_name_fragment))
      (attr_name)
      (args)
      (method_body (method_body_expr))
      (open_tag_end)
      (placeholder (placeholder_start) (placeholder_expr))
      (close_tag (close_tag_start) (close_tag_name) (close_tag_end))
      (element_end))
    (element_end)))
```

Nodes you will commonly query:

| node                                                                                       | meaning                                                      |
| ------------------------------------------------------------------------------------------ | ------------------------------------------------------------ |
| `element`                                                                                  | a tag in either mode, including its body and close           |
| `tag_name`, `tag_name_fragment`                                                            | tag names; interpolated names contain `placeholder` children |
| `shorthand_id`, `shorthand_class`                                                          | `#id` / `.class` shorthands                                  |
| `attr_name`, `attr_value`, `attr_bound_value`, `attr_spread`                               | attributes (`x=…`, `x:=…`, `...spread`)                      |
| `args`, `method_body`                                                                      | `(…)` arguments and `{ … }` shorthand-method bodies          |
| `tag_var` → `var_pattern`, `var_type`                                                      | tag variables: `/pattern` with an optional `: type`          |
| `params` → `param` → `param_pattern`, `param_type`, `param_default`                        | `\|a: T = 1, b\|` tag parameters                             |
| `type_args`, `type_params` → `type_expr`                                                   | `<T>` type arguments/parameters                              |
| `placeholder`, `placeholder_expr`                                                          | `${…}` and `$!{…}`                                           |
| `scriptlet`, `scriptlet_expr`, `scriptlet_block_expr`                                      | `$ statement` / `$ { block }`                                |
| `statement_expr`                                                                           | the body of statement tags (`import`, `static`, `server`, …) |
| `text`, `html_comment`, `line_comment`, `block_comment`, `cdata`, `doctype`, `declaration` | content                                                      |
| `html_block`                                                                               | `--` delimited / single-line content blocks in concise mode  |

A few structural notes:

- `element_end` is a zero-width node marking where an element closes
  implicitly (concise dedent, void tags, self-closing, end of input).
  `concise_open_tag_end` is similarly zero-width (or covers a trailing `;`).
- Delimiters are visible named tokens — `args_open`/`args_close`,
  `params_open`/`params_close`, `type_open`/`type_close`,
  `attr_group_open`/`attr_group_close`, `method_body_open`/
  `method_body_close`, `scriptlet_block_open`/`scriptlet_block_close`,
  `placeholder_start`/`placeholder_end` — so bracket-matching and
  punctuation queries can target them.
- Whitespace is significant in Marko, so the grammar has no `extras`; text
  nodes contain their exact whitespace.
- Invalid input produces an `ERROR` node from the first error onward —
  matching how Marko itself reports a single error per template rather than
  guessing at recovery.

## Tag types

Tag parsing in Marko depends on the tag: this grammar follows the
[tags API](https://markojs.com/docs/reference/core-tag)'s `parseOptions` (and
the vscode tmLanguage grammar it replaces):

- **void**: the html void elements plus `const`, `debug`, `id`, `let`,
  `lifecycle`, `log`, `return` — no children, no closing tag
- **raw text**: `script`, `style`, `html-script`, `html-style`,
  `html-comment` — bodies are text and placeholders only
- **statement**: `import`, `export`, `static`, `server`, `client` (plus
  `class` for class-API compatibility) — the rest of the line (and indented
  continuation) is a single embedded statement

## Embedded languages

Marko templates embed TypeScript and CSS throughout, and
`queries/injections.scm` maps it all: TypeScript is injected into every
expression position — placeholders, attribute values/spreads, arguments,
shorthand-method bodies, scriptlets, statement-tag bodies, and the
pattern/default parts of tag variables and parameters.

Statement tags split into two groups, mirroring how the compiler consumes
them: `import`, `export`, and `class` are pass-through TypeScript — the
keyword is part of the statement — so the whole element (keyword included)
is injected and the keyword takes its color from the TS grammar. For
`static`, `server`, and `client` the keyword is Marko syntax that the
compiler strips, so only the body is injected and `highlights.scm` captures
the keyword as `@keyword` (the tmLanguage grammar makes the same split).
`<script>`/`<html-script>` bodies inject TypeScript and
`<style>`/`<html-style>` bodies inject CSS — including their concise
`script --` / `style --` block forms — with `injection.combined` merging
chunks split by placeholders. `style` dialect shorthands are honored the
way the compiler resolves them: the last shorthand segment names the
injected language, so `<style.scss>` and `style.module.scss --` inject
`scss` while plain `<style>` falls back to `css` (`html-style` is exempt —
its shorthand is a real class attribute, not a dialect).

Type annotations (tag var/param types, `<T>` type args/params) are captured
as `@type` instead of injected: a bare type is not a valid TypeScript
program, so flat coloring is the accurate option (the tmLanguage grammar
makes the same approximation). One residual: the parameters of a shorthand
method (`onClick(event) { … }`) inject as a whole TS program, which renders
typed parameters slightly off — that position cannot be split without
lookahead the scanner doesn't have.

## Fidelity and development

Marko's parser is [htmljs-parser](https://github.com/marko-js/htmljs-parser);
this grammar's external scanner reimplements its state machine, and the test
suite (`npm test`, Node 22) asserts the tree reproduces the parser's event
stream with byte-precise ranges across the parser's full fixture suite —
plus thousands of templates from the Marko ecosystem during development.

The reference parser and its fixtures are always the same htmljs-parser
revision: they are fetched together into `.cache/` with degit, so the suite
compares the grammar against the exact parser the fixtures came from (set
`HTMLJS_FIXTURES` to point at a local checkout's fixtures instead, and the
adjacent sources are used as the parser; if this directory is dropped inside
an htmljs-parser checkout, the local parser sources are used automatically).
The published `htmljs-parser` package is only a last-resort fallback. The
grammar tracks the parser, so the suite is only expected to be green against
the htmljs-parser revision the grammar was synced with.

See `__tests__/` and `tools/` to work on the grammar itself; the scanner
internals are documented at the top of `src/scanner.c`.
