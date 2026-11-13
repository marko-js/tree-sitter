import assert from "node:assert";
import { makeParser } from "./util/compare.mts";
import { parseEvents, type Event } from "./util/events.mts";
import { treeEvents } from "./util/tree-events.mts";

// The fixture comparison drops every event at or after the parser's first
// error, so an event the parser still reports where its error starts is
// checked here: it must be in the tree, which must hold an error too.
const cases: { name: string; src: string; event: Event }[] = [
  {
    name: "an html mode attribute name cut off by EOF",
    src: "<div foo",
    event: { label: "attrName", start: 5, end: 8 },
  },
  {
    name: "an html mode attribute name cut off by EOF after a value",
    src: "<div a=1 foo",
    event: { label: "attrName", start: 9, end: 12 },
  },
  {
    name: "an html mode attribute name with a modifier cut off by EOF",
    src: "<a><b selected:x",
    event: { label: "attrName", start: 6, end: 16 },
  },
];

const has = (events: Event[], { label, start, end }: Event) =>
  events.some((e) => e.label === label && e.start === start && e.end === end);

describe("tree-sitter-marko events at an error", () => {
  for (const { name, src, event } of cases) {
    it(name, () => {
      const expected = parseEvents(src);
      assert.ok(
        expected.some((e) => e.label.startsWith("error(")),
        "the parser reports an error",
      );
      assert.ok(has(expected, event), "the parser reports the event");

      const tree = makeParser().parse(src);
      assert.ok(tree.rootNode.hasError, `tree has an error: ${tree.rootNode}`);
      assert.ok(
        has(treeEvents(src, tree.rootNode), event),
        `tree reports ${event.label} [${event.start},${event.end}]: ${tree.rootNode}`,
      );
    });
  }
});
