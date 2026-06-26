// Keep tree-sitter.json's metadata.version in sync with package.json after a
// `changeset version` bump, so the published grammar manifest never drifts.
import { readFileSync, writeFileSync } from "node:fs";

const { version } = JSON.parse(readFileSync("package.json", "utf8"));
const config = JSON.parse(readFileSync("tree-sitter.json", "utf8"));

if (config.metadata?.version !== version) {
  config.metadata.version = version;
  writeFileSync("tree-sitter.json", JSON.stringify(config, null, 2) + "\n");
}
