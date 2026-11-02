---
"@marko/tree-sitter": minor
---

Match htmljs-parser 5.20's concise line starts: a line starting with `-` and a word character (eg `-if(x)`, `-else-if`, `-my-tag`) opens a tag whose name starts with `-`, and a line starting with `;` is ignored, with only comments allowed after it. An open tag that ends before its name (eg a line of just `,`, or `<,>`) now gets an empty `tag_name` instead of failing to parse, and a name after a leading `,` and comments (eg `, /* note */ div`) is read as the tag name.
