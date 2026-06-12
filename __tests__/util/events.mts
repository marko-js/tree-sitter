import type { Ranges, Range } from "htmljs-parser";
import { loadHtmljs } from "./htmljs.mts";

// Works both inside the htmljs-parser repository (compares against the
// local sources) and standalone (compares against the published package).
const { createParser, ErrorCode, TagType } = await loadHtmljs();
type TagType = (typeof TagType)[keyof typeof TagType];

export interface Event {
  label: string;
  start: number;
  end: number;
}

/**
 * Runs htmljs-parser over the source and returns its event stream in a
 * normalized form. Mirrors the handlers (including the tag type lookup) used
 * by src/__tests__/main.test.ts so that matching this event stream implies
 * matching the fixture snapshots.
 */
export function parseEvents(src: string): Event[] {
  const events: Event[] = [];
  const add = (label: string, range: Range) =>
    events.push({ label, start: range.start, end: range.end });
  const addValue = (label: string, range: Ranges.Value) => {
    add(label, range);
    add(`${label}.value`, range.value);
  };
  const addTemplate = (label: string, range: Ranges.Template) => {
    const [first] = range.quasis;
    add(label, range);

    if (first.start !== range.start || first.end !== range.end) {
      add(`${label}.quasis[0]`, first);

      for (let i = 0; i < range.expressions.length; i++) {
        const j = i + 1;
        add(`${label}.quasis[${j}]`, range.quasis[j]);
        add(`${label}.expressions[${i}]`, range.expressions[i]);
      }
    }
  };

  const tagStack: { type: TagType; range: Ranges.Template }[] = [];
  const parser = createParser({
    onError(range) {
      add(`error(${ErrorCode[range.code]}:${range.message})`, range);
    },
    onText(range) {
      add("text", range);
    },
    onPlaceholder(range) {
      addValue(`placeholder${range.escape ? ":escape" : ""}`, range);
    },
    onCDATA(range) {
      addValue("cdata", range);
    },
    onDoctype(range) {
      addValue("doctype", range);
    },
    onDeclaration(range) {
      addValue("declaration", range);
    },
    onComment(range) {
      addValue("comment", range);
    },
    onOpenTagStart(range) {
      add("openTagStart", range);
    },
    onOpenTagName(range) {
      addTemplate("tagName", range);

      if (range.expressions.length === 0) {
        // Tag types follow the tags API (parseOptions of the core tags in
        // marko/packages/runtime-tags) and the vscode tmLanguage grammar:
        // html void elements + openTagOnly tags are void, text:true tags
        // are text, statement:true tags (and class) are statements.
        switch (parser.read(range)) {
          case "area":
          case "base":
          case "br":
          case "col":
          case "hr":
          case "embed":
          case "img":
          case "input":
          case "link":
          case "meta":
          case "param":
          case "source":
          case "track":
          case "wbr":
          case "const":
          case "debug":
          case "id":
          case "let":
          case "lifecycle":
          case "log":
          case "return":
            tagStack.push({ type: TagType.void, range });
            return TagType.void;
          case "script":
          case "style":
          case "html-script":
          case "html-style":
          case "html-comment":
            tagStack.push({ type: TagType.text, range });
            return TagType.text;
          case "import":
          case "export":
          case "static":
          case "class":
          case "server":
          case "client":
            tagStack.push({ type: TagType.statement, range });
            return TagType.statement;
        }
      }

      tagStack.push({ type: TagType.html, range });
      return TagType.html;
    },
    onTagShorthandId(range) {
      addTemplate("tagShorthandId", range);
    },
    onTagShorthandClass(range) {
      addTemplate("tagShorthandClass", range);
    },
    onTagTypeArgs(range) {
      addValue("tagTypeArgs", range);
    },
    onTagVar(range) {
      addValue("tagVar", range);
    },
    onTagArgs(range) {
      addValue("tagArgs", range);
    },
    onTagParams(range) {
      addValue("tagParams", range);
    },
    onTagTypeParams(range) {
      addValue("tagTypeParams", range);
    },
    onAttrName(range) {
      add("attrName", range);
    },
    onAttrArgs(range) {
      addValue("attrArgs", range);
    },
    onAttrValue(range) {
      addValue(range.bound ? `attrValue:bound` : `attrValue`, range);
    },
    onAttrMethod(range) {
      add("attrMethod", range);
      if (range.typeParams) addValue("attrMethod.typeParams", range.typeParams);
      addValue("attrMethod.params", range.params);
      addValue("attrMethod.body", range.body);
    },
    onAttrSpread(range) {
      addValue("attrSpread", range);
    },
    onOpenTagEnd(range) {
      if (range.selfClosed) tagStack.pop();
      else
        switch (tagStack[tagStack.length - 1]!.type) {
          case TagType.statement:
          case TagType.void:
            tagStack.pop();
            break;
        }
      add(`openTagEnd${range.selfClosed ? ":selfClosed" : ""}`, range);
    },
    onCloseTagStart(range) {
      add("closeTagStart", range);
    },
    onCloseTagName(range) {
      add("closeTagName", range);
    },
    onCloseTagEnd(range) {
      add(`closeTagEnd(${parser.read(tagStack.pop()!.range)})`, range);
    },
    onScriptlet(range) {
      addValue(range.block ? `scriptlet:block` : `scriptlet`, range);
    },
  });

  parser.parse(src);
  return events;
}

export function formatEvents(src: string, events: Event[]): string {
  return events
    .map(
      (e) =>
        `${e.label} [${e.start},${e.end}] ${JSON.stringify(
          src.slice(e.start, e.end),
        )}`,
    )
    .join("\n");
}
