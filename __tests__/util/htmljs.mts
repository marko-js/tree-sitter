// Resolves htmljs-parser (the reference implementation the tests compare
// against) and its test fixtures. The parser and the fixtures must always be
// the *same* htmljs-parser revision, otherwise behavioral differences between
// versions show up as spurious test failures. Three layouts are supported:
//
//  - inside the htmljs-parser repository (this directory checked out as
//    `tree-sitter/`): the parser sources and fixtures two levels up are used
//    directly, so the comparison always tracks the local parser;
//  - standalone: the fixtures *and* the parser sources are fetched together
//    into `.cache/` with degit, so they share a revision (this is what CI
//    uses); the published `htmljs-parser` package is only a last-resort
//    fallback;
//  - HTMLJS_FIXTURES override: point at any htmljs-parser `src/__tests__/
//    fixtures` directory and the adjacent `src` is used as the parser too.
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const LOCAL_SRC = path.join(here, "../../../src");
const isLocal = fs.existsSync(path.join(LOCAL_SRC, "index.ts"));
const CACHE = path.join(here, "../../.cache/htmljs-parser");

export interface HtmljsApi {
  createParser: (typeof import("htmljs-parser"))["createParser"];
  ErrorCode: (typeof import("htmljs-parser"))["ErrorCode"];
  TagType: (typeof import("htmljs-parser"))["TagType"];
}

async function importSrc(src: string): Promise<HtmljsApi> {
  // The htmljs-parser sources transpile to CJS in a way node's named-export
  // detection can't statically analyze; merge the namespaces at runtime.
  const htmljs = (await import(
    pathToFileURL(path.join(src, "index.ts")).href
  )) as any;
  const internal = (await import(
    pathToFileURL(path.join(src, "internal.ts")).href
  )) as any;
  return {
    ...(htmljs.default ?? htmljs),
    ...(internal.default ?? internal),
  } as HtmljsApi;
}

export async function loadHtmljs(): Promise<HtmljsApi> {
  // The parser source lives at `<root>/src`, fixtures at `<root>/src/
  // __tests__/fixtures`, so the parser is always two levels up from the
  // fixtures — keeping the reference parser and the fixtures in lockstep.
  const src = path.join(await fixturesDir(), "../..");
  if (fs.existsSync(path.join(src, "index.ts"))) return importSrc(src);
  // HTMLJS_FIXTURES pointed somewhere without adjacent sources: fall back to
  // the published package.
  return (await import("htmljs-parser")) as HtmljsApi;
}

export async function fixturesDir(): Promise<string> {
  if (process.env.HTMLJS_FIXTURES) return process.env.HTMLJS_FIXTURES;

  const local = path.join(LOCAL_SRC, "__tests__/fixtures");
  if (isLocal && fs.existsSync(local)) return local;

  const cached = path.join(CACHE, "src/__tests__/fixtures");
  if (!fs.existsSync(cached)) {
    const { default: degit } = await import("degit");
    await degit("marko-js/htmljs-parser").clone(CACHE);
  }
  return cached;
}
