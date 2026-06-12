; Basic syntax highlighting for Marko templates.

; Statement-tag names are not tags visually: static/server/client are
; Marko keywords (the compiler strips them before parsing the body as TS),
; while import/export/class are part of the injected TS statement itself
; and take their color from that injection (see injections.scm).
((tag_name (tag_name_fragment) @tag)
 (#not-any-of? @tag "import" "export" "class" "static" "server" "client"))
((tag_name (tag_name_fragment) @keyword)
 (#any-of? @keyword "static" "server" "client"))
(close_tag_name) @tag
(shorthand_id) @constant
(shorthand_class) @property

(attr_name) @attribute

; Binding positions: patterns get the TS injection; types are captured
; flatly because a bare type is not a valid TS program (matching the
; tmLanguage grammar's source.ts#type approximation).
(var_pattern) @variable
(var_type) @type
(param_pattern) @variable.parameter
(param_type) @type
(param_default) @none
(type_expr) @type

(open_tag_start) @punctuation.bracket
(open_tag_end) @punctuation.bracket
(open_tag_end_self) @punctuation.bracket
(close_tag_start) @punctuation.bracket
(close_tag_end) @punctuation.bracket

(args_open) @punctuation.bracket
(args_close) @punctuation.bracket
(params_open) @punctuation.bracket
(params_close) @punctuation.bracket
(type_open) @punctuation.bracket
(type_close) @punctuation.bracket
(attr_group_open) @punctuation.bracket
(attr_group_close) @punctuation.bracket
(method_body_open) @punctuation.bracket
(method_body_close) @punctuation.bracket
(scriptlet_block_open) @punctuation.bracket
(scriptlet_block_close) @punctuation.bracket

(placeholder_start) @punctuation.special
(placeholder_start_raw) @punctuation.special
(placeholder_end) @punctuation.special

(html_comment) @comment
(line_comment) @comment
(block_comment) @comment

(doctype) @keyword
(declaration) @keyword
(cdata) @string

(scriptlet_start) @punctuation.special
(scriptlet_start_concise) @punctuation.special

(text) @none
