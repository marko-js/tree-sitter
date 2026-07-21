// Usage:
//   pnpm exec tsx tools/check.mts '<div>hello</div>'   compare an inline source
//   pnpm exec tsx tools/check.mts --fixture attr-complex  compare a fixture
import fs from "node:fs";
import path from "node:path";
import { compare } from "../__tests__/util/compare.mts";
import { formatEvents } from "../__tests__/util/events.mts";
import { fixturesDir } from "../__tests__/util/htmljs.mts";

let src: string;
if (process.argv[2] === "--file") {
  src = fs.readFileSync(process.argv[3], "utf-8");
} else if (process.argv[2] === "--fixture") {
  src = fs.readFileSync(
    path.join(await fixturesDir(), process.argv[3], "input.marko"),
    "utf-8",
  );
} else {
  src = process.argv[2];
}

const result = compare(src);
console.log("source:", JSON.stringify(src));
console.log("tree:", result.tree);
console.log("error case:", result.isErrorCase);
console.log();
console.log("== parser events ==");
console.log(formatEvents(src, result.expected));
console.log();
console.log("== tree-sitter events ==");
console.log(formatEvents(src, result.actual));
console.log();
console.log(result.ok ? "OK" : `MISMATCH: ${result.message}`);
process.exitCode = result.ok ? 0 : 1;
