// Worker for run-corpus: compares files from a manifest sequentially,
// printing one flushed result line per file.
import fs from "node:fs";
import { compare } from "../__tests__/util/compare.mts";

const manifest = process.argv[2];
const startIndex = Number(process.argv[3] ?? 0);
const files = fs.readFileSync(manifest, "utf-8").split("\n").filter(Boolean);

for (let i = startIndex; i < files.length; i++) {
  const file = files[i];
  process.stdout.write(`START ${i} ${file}\n`);
  let line: string;
  try {
    const src = fs.readFileSync(file, "utf-8");
    const result = compare(src);
    line = result.ok
      ? `PASS ${i} ${file}`
      : `FAIL ${i} ${file}: ${result.message?.split("\n")[0] ?? ""}`;
  } catch (err) {
    line = `FAIL ${i} ${file}: threw ${(err as Error).message}`;
  }
  process.stdout.write(`${line}\n`);
}
process.stdout.write("DONE\n");
