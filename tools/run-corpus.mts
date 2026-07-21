// Compares every .marko file under the given directories (excluding
// node_modules) against htmljs-parser, in a watchdogged worker process so
// scanner hangs surface as HANG instead of freezing the run.
//
// Usage: pnpm exec tsx tools/run-corpus.mts [--fail-only] <dir> [<dir> ...]
import { spawn } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const args = process.argv.slice(2);
const failOnly = args.includes("--fail-only");
const dirs = args.filter((a) => !a.startsWith("--"));
const WATCHDOG_MS = 10_000;

const files: string[] = [];
const walk = (dir: string) => {
  let entries;
  try {
    entries = fs.readdirSync(dir, { withFileTypes: true });
  } catch {
    return;
  }
  for (const entry of entries) {
    if (entry.name === "node_modules" || entry.name.startsWith(".")) continue;
    const full = path.join(dir, entry.name);
    if (entry.isDirectory()) walk(full);
    else if (entry.isFile() && entry.name.endsWith(".marko")) files.push(full);
  }
};
for (const dir of dirs) walk(dir);
files.sort();
console.log(`${files.length} templates from ${dirs.join(", ")}`);

const manifest = path.join(os.tmpdir(), `tsmarko-corpus-${process.pid}.txt`);
fs.writeFileSync(manifest, files.join("\n"));

const tally = { pass: 0, fail: 0, hang: 0 };

function runWorker(startIndex: number): Promise<void> {
  return new Promise((resolve) => {
    const child = spawn(
      process.execPath,
      [
        "--import=tsx",
        path.join(here, "corpus-worker.mts"),
        manifest,
        String(startIndex),
      ],
      { stdio: ["ignore", "pipe", "inherit"], cwd: path.join(here, "..") },
    );

    let lastStart: { index: number; entry: string } | null = null;
    let buffered = "";
    let done = false;
    let timer: NodeJS.Timeout;

    const resetWatchdog = () => {
      clearTimeout(timer);
      timer = setTimeout(() => child.kill("SIGKILL"), WATCHDOG_MS);
    };
    resetWatchdog();

    child.stdout.on("data", (chunk: Buffer) => {
      resetWatchdog();
      buffered += chunk.toString();
      let nl;
      while ((nl = buffered.indexOf("\n")) !== -1) {
        const line = buffered.slice(0, nl);
        buffered = buffered.slice(nl + 1);
        if (line.startsWith("START ")) {
          const idx = line.indexOf(" ", 6);
          lastStart = {
            index: Number(line.slice(6, idx)),
            entry: line.slice(idx + 1),
          };
        } else if (line.startsWith("PASS ")) {
          tally.pass++;
          if (!failOnly) console.log(line);
        } else if (line.startsWith("FAIL ")) {
          tally.fail++;
          console.log(line);
        } else if (line === "DONE") {
          done = true;
        }
      }
    });

    child.on("exit", () => {
      clearTimeout(timer);
      if (done || !lastStart) {
        resolve();
        return;
      }
      tally.hang++;
      console.log(`HANG ${lastStart.index} ${lastStart.entry}`);
      resolve(runWorker(lastStart.index + 1));
    });
  });
}

await runWorker(0);
fs.rmSync(manifest, { force: true });
console.log(`\n${tally.pass} passed, ${tally.fail} failed, ${tally.hang} hung`);
