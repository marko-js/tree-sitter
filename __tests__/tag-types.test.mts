import assert from "node:assert";
import { compare } from "./util/compare.mts";

// Each tag type's body, where text and html read differently (the upstream
// fixtures do not tell them apart for every tag).
const cases: { name: string; src: string }[] = [
  {
    name: "a textarea body is text",
    src: "<textarea><b>x</b> ${y} <!-- z --></textarea>\n",
  },
  {
    name: "a concise textarea body is text",
    src: "textarea\n  -- <b>x</b> ${y}\np\n",
  },
];

describe("tree-sitter-marko tag types", () => {
  for (const { name, src } of cases) {
    it(name, () => {
      const result = compare(src);
      if (!result.ok) assert.fail(`${result.message}\n\ntree:\n${result.tree}`);
    });
  }
});
