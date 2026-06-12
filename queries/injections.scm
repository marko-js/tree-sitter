; Embedded-language injections for Marko templates.
;
; All expression positions in Marko hold TypeScript expressions/statements
; (TypeScript is a superset of JavaScript, so it always applies), and
; style/script tag bodies hold CSS/TS. These queries let editors parse that
; embedded content with the right grammar.

; ${...} / $!{...} placeholders (content + tag name interpolations)
((placeholder_expr) @injection.content
 (#set! injection.language "typescript"))

; $ scriptlets: `$ statement` and `$ { block }`
((scriptlet_expr) @injection.content
 (#set! injection.language "typescript"))
((scriptlet_block_expr) @injection.content
 (#set! injection.language "typescript"))

; Statement tags come in two groups, mirroring how the compiler's core tag
; parsers consume them. import/export/class are pass-through TypeScript:
; the keyword itself is part of the statement the compiler parses, so the
; injection covers the whole element (its trailing tokens are zero-width,
; making its extent exactly the statement text, semicolon included).
; include-children is required: child node ranges are otherwise excluded
; from an injection, and the element's children span its entire text.
((element
   (tag_name) @_name
   (#any-of? @_name "import" "export" "class")
   (statement_expr)) @injection.content
 (#set! injection.language "typescript")
 (#set! injection.include-children))

; static/server/client strip their keyword before the body is parsed as
; TS, so only the body is injected (the keyword is highlighted as a Marko
; keyword in highlights.scm).
(element
  (tag_name) @_name
  (#any-of? @_name "static" "server" "client")
  (statement_expr) @injection.content
  (#set! injection.language "typescript"))

; attribute values, spreads and bound values
((attr_value_expr) @injection.content
 (#set! injection.language "typescript"))

; tag variables and parameters are binding positions: their patterns and
; defaults parse cleanly as bare programs, while type annotations are
; captured flatly in highlights.scm (a bare type is not a valid TS program).
((var_pattern) @injection.content
 (#set! injection.language "typescript"))
((param_pattern) @injection.content
 (#set! injection.language "typescript"))
((param_default) @injection.content
 (#set! injection.language "typescript"))

; tag/attr arguments and shorthand-method bodies are expression/statement
; positions.
((args_expr) @injection.content
 (#set! injection.language "typescript"))
((method_body_expr) @injection.content
 (#set! injection.language "typescript"))

; <script> bodies: injection.combined merges the raw text chunks split by
; ${...} placeholders.
(element
  (tag_name) @_tag
  (#eq? @_tag "script")
  (text) @injection.content
  (#set! injection.language "typescript")
  (#set! injection.combined))

(element
  (tag_name) @_tag
  (#eq? @_tag "html-script")
  (text) @injection.content
  (#set! injection.language "typescript")
  (#set! injection.combined))

; <style> bodies are CSS by default, but shorthand "extension" segments
; pick the stylesheet dialect, last segment winning (<style.scss>,
; <style.module.scss> -> scss), mirroring the compiler's STYLE_EXT_REG.
; The @injection.language capture overrides the #set! fallback, and with
; several captured segments the last capture wins.
(element
  (tag_name) @_tag
  (#eq? @_tag "style")
  (shorthand_class (tag_name_fragment) @injection.language)*
  (text) @injection.content
  (#set! injection.language "css")
  (#set! injection.combined))

(element
  (tag_name) @_tag
  (#eq? @_tag "html-style")
  (text) @injection.content
  (#set! injection.language "css")
  (#set! injection.combined))

; style -- css blocks / script -- js blocks in concise mode (the style
; form supports the same dialect shorthands: style.scss -- ...).
;
; Unlike the <script>/<style> forms above, these omit injection.combined: the
; concise body is a nested html_block whose lines are separate text nodes, and
; Zed desyncs combined injections nested under an html_block on incremental
; edits (highlighting blanks out while typing). Per-chunk injection highlights
; each line independently, which is fine for these short blocks.
(element
  (tag_name) @_tag
  (#eq? @_tag "style")
  (shorthand_class (tag_name_fragment) @injection.language)*
  (html_block
    (text) @injection.content)
  (#set! injection.language "css"))

(element
  (tag_name) @_tag
  (#eq? @_tag "script")
  (html_block
    (text) @injection.content)
  (#set! injection.language "typescript"))

; html comments
((html_comment) @injection.content
 (#set! injection.language "html"))
