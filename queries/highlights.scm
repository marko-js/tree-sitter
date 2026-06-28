; Basic syntax highlighting for Marko templates.

; Statement-tag names are not tags visually: static/server/client are
; Marko keywords (the compiler strips them before parsing the body as TS),
; while import/export/class are part of the injected TS statement itself
; and take their color from that injection (see injections.scm).
; Native HTML element names get their own highlight so they can be themed
; apart from userland/component tags. The list mirrors @marko/language-tools'
; builtinTagsRegex (isHTMLTag), which is what the compiler tooling itself uses
; to decide whether a tag is a native element or a custom tag; keep the two in
; sync. Open and close tag names share each rule via an alternation, and the
; native/custom split is expressed with mutually exclusive predicates (rather
; than relying on match precedence, which differs between query consumers).
; Everything not in the list (and not a statement/keyword tag) keeps @tag.
([
  (tag_name (tag_name_fragment) @tag.builtin)
  (close_tag_name) @tag.builtin
 ]
 (#any-of? @tag.builtin
  "a" "abbr" "acronym" "address" "applet" "area" "article" "aside" "audio" "b"
  "base" "basefont" "bdi" "bdo" "bgsound" "big" "blink" "blockquote" "body" "br"
  "button" "canvas" "caption" "center" "cite" "code" "col" "colgroup" "command" "content"
  "data" "datalist" "dd" "del" "details" "dfn" "dialog" "dir" "div" "dl"
  "dt" "element" "em" "embed" "fieldset" "figcaption" "figure" "font" "footer" "form"
  "frame" "frameset" "h1" "h2" "h3" "h4" "h5" "h6" "head" "header"
  "hgroup" "hr" "html" "i" "iframe" "image" "img" "input" "ins" "isindex"
  "kbd" "keygen" "label" "legend" "li" "link" "listing" "main" "map" "mark"
  "marquee" "math" "menu" "menuitem" "meta" "meter" "multicol" "nav" "nextid" "nobr"
  "noembed" "noframes" "noscript" "object" "ol" "optgroup" "option" "output" "p" "param"
  "picture" "plaintext" "pre" "progress" "q" "rb" "rbc" "rp" "rt" "rtc"
  "ruby" "s" "samp" "script" "section" "select" "shadow" "slot" "small" "source"
  "spacer" "span" "strike" "strong" "style" "sub" "summary" "sup" "svg" "table"
  "tbody" "td" "template" "textarea" "tfoot" "th" "thead" "time" "title" "tr"
  "track" "tt" "u" "ul" "var" "video" "wbr" "xmp"))
([
  (tag_name (tag_name_fragment) @tag)
  (close_tag_name) @tag
 ]
 (#not-any-of? @tag
  "a" "abbr" "acronym" "address" "applet" "area" "article" "aside" "audio" "b"
  "base" "basefont" "bdi" "bdo" "bgsound" "big" "blink" "blockquote" "body" "br"
  "button" "canvas" "caption" "center" "cite" "code" "col" "colgroup" "command" "content"
  "data" "datalist" "dd" "del" "details" "dfn" "dialog" "dir" "div" "dl"
  "dt" "element" "em" "embed" "fieldset" "figcaption" "figure" "font" "footer" "form"
  "frame" "frameset" "h1" "h2" "h3" "h4" "h5" "h6" "head" "header"
  "hgroup" "hr" "html" "i" "iframe" "image" "img" "input" "ins" "isindex"
  "kbd" "keygen" "label" "legend" "li" "link" "listing" "main" "map" "mark"
  "marquee" "math" "menu" "menuitem" "meta" "meter" "multicol" "nav" "nextid" "nobr"
  "noembed" "noframes" "noscript" "object" "ol" "optgroup" "option" "output" "p" "param"
  "picture" "plaintext" "pre" "progress" "q" "rb" "rbc" "rp" "rt" "rtc"
  "ruby" "s" "samp" "script" "section" "select" "shadow" "slot" "small" "source"
  "spacer" "span" "strike" "strong" "style" "sub" "summary" "sup" "svg" "table"
  "tbody" "td" "template" "textarea" "tfoot" "th" "thead" "time" "title" "tr"
  "track" "tt" "u" "ul" "var" "video" "wbr" "xmp"
  "import" "export" "class" "static" "server" "client"))
((tag_name (tag_name_fragment) @keyword)
 (#any-of? @keyword "static" "server" "client"))
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
