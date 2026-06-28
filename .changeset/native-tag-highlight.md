---
"@marko/tree-sitter": minor
---

Highlight native HTML element names with a dedicated `@tag.builtin` capture so they can be themed apart from custom/component tags. The element list mirrors `@marko/language-tools`' `isHTMLTag`, which is what the compiler tooling uses to distinguish native tags from custom tags.
