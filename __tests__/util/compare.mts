import Parser from "tree-sitter";
import { createRequire } from "node:module";
import { parseEvents, type Event } from "./events.mts";
import { treeEvents, sortEvents } from "./tree-events.mts";

const require = createRequire(import.meta.url);
// eslint-disable-next-line @typescript-eslint/no-var-requires
const Marko = require("../../bindings/node");

// A fresh parser per parse: a timed-out parse can poison subsequent parses
// on the same instance.
function makeParser() {
  const parser = new Parser();
  parser.setLanguage(Marko);
  // A scanner bug must fail the test, not hang the suite.
  (parser as any).setTimeoutMicros?.(5_000_000);
  return parser;
}

export interface CompareResult {
  ok: boolean;
  isErrorCase: boolean;
  message?: string;
  expected: Event[];
  actual: Event[];
  tree: string;
}

const fmt = (e: Event) => `${e.label} [${e.start},${e.end}]`;

/**
 * Compares htmljs-parser's event stream against the tree-sitter parse.
 *
 * - Clean inputs: the tree must have no errors and both event multisets must
 *   match exactly.
 * - Error inputs (the parser stops at its first error): the tree must contain
 *   an error, and all events that end at or before the error's start must
 *   match.
 */
export function compare(src: string): CompareResult {
  const expectedAll = parseEvents(src);
  let tree;
  try {
    tree = makeParser().parse(src);
  } catch (err) {
    tree = null;
  }
  if (!tree) {
    return {
      ok: false,
      isErrorCase: false,
      message: "tree-sitter parse timed out (scanner hang?)",
      expected: expectedAll,
      actual: [],
      tree: "<timeout>",
    };
  }
  const actualAll = treeEvents(src, tree.rootNode);

  const errorEvent = expectedAll.find((e) => e.label.startsWith("error("));
  const isErrorCase = errorEvent !== undefined;

  let expected: Event[];
  let actual: Event[];

  if (isErrorCase) {
    const limit = errorEvent.start;
    expected = expectedAll.filter(
      (e) => !e.label.startsWith("error(") && e.end <= limit,
    );
    actual = actualAll.filter((e) => e.end <= limit);
  } else {
    expected = expectedAll;
    actual = actualAll;
  }

  // The scanner may split text tokens at any point (eg around newlines in
  // delimited blocks); the parser never emits adjacent text events, so
  // merging them on both sides is a sound normalization.
  const mergeText = (events: Event[]): Event[] => {
    const out: Event[] = [];
    for (const e of events) {
      const prev = out[out.length - 1];
      if (
        prev &&
        prev.label === "text" &&
        e.label === "text" &&
        prev.end === e.start
      ) {
        prev.end = e.end;
      } else {
        out.push({ ...e });
      }
    }
    return out;
  };

  expected = mergeText(sortEvents(expected));
  actual = mergeText(sortEvents(actual));

  // When every token fails (the scanner refuses even during recovery),
  // tree-sitter yields a childless document with hasError === false; treat
  // "the tree's tokens stop at/before the error position" as having errored.
  const lastTokenEnd = (node: any): number => {
    if (node.childCount === 0) {
      return node.type === "document" ? 0 : node.endIndex;
    }
    let max = 0;
    for (let i = 0; i < node.childCount; i++) {
      max = Math.max(max, lastTokenEnd(node.child(i)));
    }
    return max;
  };

  let message: string | undefined;
  if (
    isErrorCase &&
    !tree.rootNode.hasError &&
    lastTokenEnd(tree.rootNode) > errorEvent.start
  ) {
    message = `expected a parse error (${errorEvent.label}) but the tree is clean`;
  } else if (!isErrorCase && tree.rootNode.hasError) {
    message = "tree has ERROR/MISSING nodes but the parser reported none";
  } else {
    const a = expected.map(fmt);
    const b = actual.map(fmt);
    if (a.join("\n") !== b.join("\n")) {
      const diff: string[] = [];
      const max = Math.max(a.length, b.length);
      for (let i = 0, j = 0; i < max || j < max; ) {
        const ea = a[i];
        const eb = b[j];
        if (ea === eb) {
          i++;
          j++;
          continue;
        }
        if (ea !== undefined && (eb === undefined || !b.includes(ea, j))) {
          diff.push(`- ${ea}`);
          i++;
        } else if (eb !== undefined) {
          diff.push(`+ ${eb}`);
          j++;
        } else {
          break;
        }
        if (diff.length > 24) {
          diff.push("...");
          break;
        }
      }
      message = `event streams differ (- parser / + tree-sitter):\n${diff.join("\n")}`;
    }
  }

  return {
    ok: message === undefined,
    isErrorCase,
    message,
    expected,
    actual,
    tree: tree.rootNode.toString(),
  };
}
