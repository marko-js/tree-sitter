import assert from "node:assert";
import fs from "node:fs";
import path from "node:path";
import { compare } from "./util/compare.mts";
import { fixturesDir } from "./util/htmljs.mts";

const FIXTURES = await fixturesDir();

describe("tree-sitter-marko fixtures", () => {
  for (const entry of fs.readdirSync(FIXTURES)) {
    if (entry.endsWith(".skip")) {
      it.skip(entry.slice(0, -".skip".length));
      continue;
    }

    it(entry, () => {
      const src = fs.readFileSync(
        path.join(FIXTURES, entry, "input.marko"),
        "utf-8",
      );
      const result = compare(src);
      if (!result.ok) {
        assert.fail(`${result.message}\n\ntree:\n${result.tree}`);
      }
    });
  }
});
