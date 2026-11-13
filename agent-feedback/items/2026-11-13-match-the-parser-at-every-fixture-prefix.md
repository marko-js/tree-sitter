---
type: bug
impact: low
effort: med
site: __tests__/fixtures.test.mts › describe("tree-sitter-marko fixtures")
---

# Match htmljs-parser at an EOF anywhere in a fixture

htmljs-parser 5.22's own suite also runs every prefix of every fixture (an EOF at each position) and a CRLF copy of each. Comparing the prefixes through `compare` in `__tests__/util/compare.mts`, about 930 of some 33k fail: events the parser reports just before its EOF error are missing from the tree, mostly in shorthand method, tag type parameter and tag var fixtures (args or types cut off by EOF) and delimited blocks ending in a whitespace-only line. These are what an editor sees while typing at the end of a file. Fix the scanner cases, then add a prefix and CRLF pass to the suite so they stay fixed.

Check: `pnpm exec tsx tools/check.mts $'<div a, (\n'` reports `- attrName [8,8]`, `pnpm exec tsx tools/check.mts $'<foo <A>|\n'` reports `- tagTypeParams [5,8]`, and `pnpm exec tsx tools/check.mts $'div\n    span\n        ---\n \n'` reports `- text [25,26]`.
