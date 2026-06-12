// Smoke test for the wasm build: loads tree-sitter-marko.wasm with
// web-tree-sitter, parses a sample exercising both modes, and runs the
// bundled queries against it.
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { Language, Parser, Query } from "web-tree-sitter";

const root = path.join(path.dirname(fileURLToPath(import.meta.url)), "..");

await Parser.init();
const Marko = await Language.load(path.join(root, "tree-sitter-marko.wasm"));
const parser = new Parser();
parser.setLanguage(Marko);

const src = `import Button from "./button.marko"

<let/count: number = 0/>
div.panel
  <button onClick() { count++ }>\${count}</button>
style -- .panel { color: red }
`;

const tree = parser.parse(src);
if (!tree) throw new Error("parse returned null");
if (tree.rootNode.hasError) {
  throw new Error(`wasm parse has errors:\n${tree.rootNode.toString()}`);
}

for (const file of [
  "queries/injections.scm",
  "queries/highlights.scm",
]) {
  const query = new Query(
    Marko,
    fs.readFileSync(path.join(root, file), "utf-8"),
  );
  const matches = query.matches(tree.rootNode);
  if (file.includes("injections") && matches.length === 0) {
    throw new Error(`${file} produced no matches`);
  }
}

console.log("wasm OK:", tree.rootNode.toString().slice(0, 80) + "…");
