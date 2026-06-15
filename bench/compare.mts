// Benchmarks the native tree-sitter Marko parser against the reference
// JavaScript implementation (htmljs-parser) over the shared test-suite
// fixtures.
//
// Both parsers do the same job: turn Marko source into a parse result.
//  - htmljs-parser is an event/streaming parser, so the idiomatic "parse" is
//    `createParser(handlers).parse(src)`. We pass empty handlers — the parser
//    does all of its work, the consumer does nothing — the cheapest possible
//    consumption, analogous to tree-sitter handing back a tree no one reads.
//  - tree-sitter parses into a lazily materialized, C-side tree. `parse()`
//    alone is the closest analogue to the above. Because a tree is only useful
//    once read from JS, we also report "parse + full tree walk", which visits
//    every node via a cursor — comparable to actually consuming the parse.
//
// A small hand-rolled harness (warmup + repeated timed rounds, reported by the
// best/median round) is used instead of a statistics library so the benchmark
// has no extra dependencies and is trivially reproducible.
//
// Run with:  npm run bench    (from the tree-sitter repo)
import { createRequire } from "node:module";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

import Parser from "tree-sitter";

import { fixturesDir } from "../__tests__/util/htmljs.mts";

const require = createRequire(import.meta.url);
// eslint-disable-next-line @typescript-eslint/no-var-requires
const Marko = require("../bindings/node");

const here = path.dirname(fileURLToPath(import.meta.url));

// Prefer the locally built htmljs-parser dist (same revision as the fixtures),
// fall back to the installed package.
const localDist = path.join(here, "../../htmljs-parser/dist/index.mjs");
const usingLocal = fs.existsSync(localDist);
const htmljsMod = usingLocal
  ? await import(pathToFileURL(localDist).href)
  : await import("htmljs-parser");
// The installed package is CJS; its named exports may sit under `default`.
const htmljs = (
  htmljsMod.createParser ? htmljsMod : (htmljsMod.default ?? htmljsMod)
) as typeof import("htmljs-parser");

const FIXTURES = await fixturesDir();
const cases = fs
  .readdirSync(FIXTURES)
  .filter((entry) => !entry.endsWith(".skip"))
  .map((entry) => ({
    name: entry,
    src: fs.readFileSync(path.join(FIXTURES, entry, "input.marko"), "utf-8"),
  }))
  .filter((c) => c.src.length > 0);

const totalBytes = cases.reduce((n, c) => n + Buffer.byteLength(c.src), 0);
const mbTotal = totalBytes / 1e6;

// Visit every node of a parsed tree using a cursor (iterative DFS), forcing the
// lazy C-side tree to be fully materialized into JS.
function walkTree(tree: Parser.Tree): number {
  const cursor = tree.walk();
  let count = 1;
  for (;;) {
    if (cursor.gotoFirstChild()) {
      count++;
      continue;
    }
    for (;;) {
      if (cursor.gotoNextSibling()) {
        count++;
        break;
      }
      if (!cursor.gotoParent()) return count;
    }
  }
}

// The work units. Each parses the whole fixture suite exactly once.
const htmljsParse = () => {
  for (const c of cases) htmljs.createParser({}).parse(c.src);
};

const tsReused = (() => {
  const parser = new Parser();
  parser.setLanguage(Marko);
  return () => {
    for (const c of cases) parser.parse(c.src);
  };
})();

const tsFresh = () => {
  for (const c of cases) {
    const parser = new Parser();
    parser.setLanguage(Marko);
    parser.parse(c.src);
  }
};

const tsReusedWalk = (() => {
  const parser = new Parser();
  parser.setLanguage(Marko);
  return () => {
    for (const c of cases) walkTree(parser.parse(c.src));
  };
})();

interface Bench {
  label: string;
  fn: () => void;
}

const benches: Bench[] = [
  { label: "htmljs-parser  (parse, empty handlers)", fn: htmljsParse },
  { label: "tree-sitter    (parse, reused parser)", fn: tsReused },
  { label: "tree-sitter    (parse, fresh parser/file)", fn: tsFresh },
  { label: "tree-sitter    (parse + full tree walk)", fn: tsReusedWalk },
];

// Run `fn` `rounds` times, returning the per-round suite time in ms. Each round
// parses all fixtures once. A warmup phase lets the JIT settle first.
function measure(fn: () => void, warmup = 30, rounds = 120): number[] {
  for (let i = 0; i < warmup; i++) fn();
  const times: number[] = [];
  for (let i = 0; i < rounds; i++) {
    const t = process.hrtime.bigint();
    fn();
    times.push(Number(process.hrtime.bigint() - t) / 1e6);
  }
  return times.sort((a, b) => a - b);
}

const median = (sorted: number[]) => sorted[Math.floor(sorted.length / 2)];

console.log(
  `Marko parser benchmark\n` +
    `  fixtures : ${cases.length} files, ${totalBytes.toLocaleString()} bytes total\n` +
    `  parsers  : htmljs-parser ${
      usingLocal ? "(local dist)" : "(installed package)"
    } vs @marko/tree-sitter (native binding)\n` +
    `  method   : best & median of 120 timed rounds (each round parses every fixture once)\n` +
    `  node     : ${process.version}\n`,
);

const results = benches.map((b) => {
  const times = measure(b.fn);
  const best = times[0];
  const med = median(times);
  return { label: b.label, best, med };
});

const baseline = results[0].best; // htmljs-parser best round

const col = (s: string, w: number) => s.padStart(w);
console.log(
  "  " +
    "parser".padEnd(42) +
    col("best", 10) +
    col("median", 10) +
    col("MB/s", 9) +
    col("vs JS", 9),
);
console.log("  " + "-".repeat(42 + 10 + 10 + 9 + 9));
for (const r of results) {
  const mbPerSec = mbTotal / (r.best / 1e3);
  const rel = baseline / r.best; // >1 = faster than htmljs-parser
  console.log(
    "  " +
      r.label.padEnd(42) +
      col(`${r.best.toFixed(2)}ms`, 10) +
      col(`${r.med.toFixed(2)}ms`, 10) +
      col(mbPerSec.toFixed(1), 9) +
      col(`${rel.toFixed(2)}x`, 9),
  );
}
console.log(
  `\n  (best round = fastest of 120; "vs JS" > 1.0 means faster than htmljs-parser)`,
);
