import type { SyntaxNode } from "tree-sitter";
import type { Event } from "./events.mts";

// Tag types follow the tags API / vscode tmLanguage grammar (see
// classify_tag_name in ../../src/scanner.c).
const VOID_TAGS = new Set([
  "area",
  "base",
  "br",
  "col",
  "hr",
  "embed",
  "img",
  "input",
  "link",
  "meta",
  "param",
  "source",
  "track",
  "wbr",
  "const",
  "debug",
  "id",
  "let",
  "lifecycle",
  "log",
  "return",
]);
// Note: no TEXT_TAGS set here. Text-ness only affects how the scanner lexes
// tag bodies; for event reconstruction, concise text tags close exactly like
// html tags (a real closeTagEnd at the dedent), so only void and statement
// tags need handling (they suppress the implicit close).
const STATEMENT_TAGS = new Set([
  "import",
  "export",
  "static",
  "class",
  "server",
  "client",
]);

/**
 * Reconstructs htmljs-parser's event stream from a tree-sitter parse tree.
 * The grammar's nodes carry enough information (token kinds + ranges) that
 * every parser event maps to a deterministic function of the tree + source.
 */
export function treeEvents(src: string, root: SyntaxNode): Event[] {
  const events: Event[] = [];
  const add = (label: string, start: number, end: number) =>
    events.push({ label, start, end });
  const addValue = (
    label: string,
    start: number,
    end: number,
    vs: number,
    ve: number,
  ) => {
    add(label, start, end);
    add(`${label}.value`, vs, ve);
  };

  // Template events (tag names & shorthands): quasis/expressions derived
  // from placeholder children.
  // addTemplateRange: quasi sub-events print whenever quasis[0] differs from
  // the whole range (shorthands always differ: quasis start after the #/.).
  const addTemplate = (
    label: string,
    start: number,
    end: number,
    placeholders: SyntaxNode[],
    quasiStart = start,
  ) => {
    add(label, start, end);
    if (placeholders.length === 0 && quasiStart === start) return;
    add(
      `${label}.quasis[0]`,
      quasiStart,
      placeholders.length ? placeholders[0].startIndex : end,
    );
    for (let i = 0; i < placeholders.length; i++) {
      const ph = placeholders[i];
      const nextStart =
        i + 1 < placeholders.length ? placeholders[i + 1].startIndex : end;
      add(`${label}.quasis[${i + 1}]`, ph.endIndex, nextStart);
      add(`${label}.expressions[${i}]`, ph.startIndex, ph.endIndex);
    }
  };

  // closeTagEnd position for implicit (dedent/EOF) closes: the parser's
  // `indentStart = pos - curIndent - 1` formula recomputed from the source.
  const implicitClosePos = (from: number): number => {
    let n = from;
    while (n < src.length && src.charCodeAt(n) <= 32) n++;
    if (n >= src.length) return src.length;
    // the newline before the dedenting line's indent
    return Math.max(src.lastIndexOf("\n", n), 0);
  };

  const childrenOf = (node: SyntaxNode): SyntaxNode[] => {
    const out: SyntaxNode[] = [];
    for (let i = 0; i < node.childCount; i++) {
      const c = node.child(i);
      if (c) out.push(c);
    }
    return out;
  };

  const visitPlaceholder = (node: SyntaxNode) => {
    // Content placeholder: ${expr} (escape) or $!{expr} (no escape).
    const first = node.child(0);
    const escape = first?.type !== "placeholder_start_raw";
    const label = escape ? "placeholder:escape" : "placeholder";
    addValue(
      label,
      node.startIndex,
      node.endIndex,
      node.startIndex + (escape ? 2 : 3),
      node.endIndex - 1,
    );
  };

  const templateParts = (node: SyntaxNode) =>
    childrenOf(node).filter((c) => c.type === "placeholder");

  interface Attr {
    name?: SyntaxNode;
    typeParams?: SyntaxNode;
    args?: SyntaxNode;
  }

  const innerValue = (node: SyntaxNode, exprType: string): [number, number] => {
    const expr = childrenOf(node).find((c) => c.type === exprType);
    if (expr) return [expr.startIndex, expr.endIndex];
    return [node.startIndex + 1, node.endIndex - 1];
  };

  const visitElement = (node: SyntaxNode) => {
    const kids = childrenOf(node);
    let tagNameNode: SyntaxNode | undefined;
    let openEnd: SyntaxNode | undefined;
    let closeTag: SyntaxNode | undefined;
    let cur: Attr | null = null;
    let pendingTypes: SyntaxNode | undefined;

    // attrArgs fires for args not followed by a method body.
    const flushArgs = () => {
      if (cur?.args) {
        const a = cur.args;
        const [vs, ve] = innerValue(a, "args_expr");
        addValue("attrArgs", a.startIndex, a.endIndex, vs, ve);
        cur.args = undefined;
      }
    };

    const flushAttr = () => {
      flushArgs();
      cur = null;
    };

    const ensureAttrName = (at: number) => {
      if (!cur?.name) add("attrName", at, at);
    };

    // A comma between tag parts ends the current attribute (the parser's
    // OPEN_TAG consumes commas between attributes).
    let curEnd = -1;
    const commaBefore = (part: SyntaxNode): boolean =>
      cur !== null &&
      curEnd >= 0 &&
      src.slice(curEnd, part.startIndex).includes(",");

    const visitTagPart = (part: SyntaxNode) => {
      // Group delimiters are visible tokens but emit no events and must not
      // advance curEnd: comma detection spans across the "[" exactly like the
      // parser's OPEN_TAG state, which consumes the bracket silently.
      if (part.type === "attr_group_open" || part.type === "attr_group_close") {
        return;
      }
      if (
        cur &&
        commaBefore(part) &&
        (part.type === "args" ||
          part.type === "attr_value" ||
          part.type === "attr_bound_value" ||
          part.type === "type_params" ||
          part.type === "method_body")
      ) {
        flushAttr();
      }
      switch (part.type) {
        case "attr_name":
          flushAttr();
          cur = { name: part };
          add("attrName", part.startIndex, part.endIndex);
          break;
        case "type_args": {
          const [vs, ve] = innerValue(part, "type_expr");
          addValue("tagTypeArgs", part.startIndex, part.endIndex, vs, ve);
          break;
        }
        case "type_params":
          if (cur) {
            cur.typeParams = part;
          } else {
            pendingTypes = part;
          }
          break;
        case "params": {
          // tag params; a preceding type_params is tagTypeParams.
          if (pendingTypes) {
            const [vs, ve] = innerValue(pendingTypes, "type_expr");
            addValue(
              "tagTypeParams",
              pendingTypes.startIndex,
              pendingTypes.endIndex,
              vs,
              ve,
            );
            pendingTypes = undefined;
          }
          const [vs, ve] = innerValue(part, "params_expr");
          addValue("tagParams", part.startIndex, part.endIndex, vs, ve);
          break;
        }
        case "args": {
          if (cur) {
            cur.args = part;
          } else if (pendingTypes) {
            cur = { typeParams: pendingTypes, args: part };
            pendingTypes = undefined;
          } else if (part.nextNamedSibling?.type === "method_body") {
            cur = { args: part };
          } else if (tagPartsSeenAttr) {
            // an unnamed attribute starting with args: ensureAttrName fires
            // when ATTRIBUTE sees the "("
            add("attrName", part.startIndex, part.startIndex);
            cur = { args: part };
          } else {
            const [vs, ve] = innerValue(part, "args_expr");
            addValue("tagArgs", part.startIndex, part.endIndex, vs, ve);
          }
          break;
        }
        case "method_body": {
          // attrMethod
          const a: Attr = cur ?? {};
          const start = a.typeParams?.startIndex ?? a.args!.startIndex;
          ensureAttrName(start);
          add("attrMethod", start, part.endIndex);
          if (a.typeParams) {
            const [vs, ve] = innerValue(a.typeParams, "type_expr");
            addValue(
              "attrMethod.typeParams",
              a.typeParams.startIndex,
              a.typeParams.endIndex,
              vs,
              ve,
            );
          }
          if (a.args) {
            const [vs, ve] = innerValue(a.args, "args_expr");
            addValue(
              "attrMethod.params",
              a.args.startIndex,
              a.args.endIndex,
              vs,
              ve,
            );
          }
          const [bs, be] = innerValue(part, "method_body_expr");
          addValue("attrMethod.body", part.startIndex, part.endIndex, bs, be);
          cur = null;
          break;
        }
        case "attr_value":
        case "attr_bound_value": {
          // attrArgs flushes but the attribute (and its name) continues:
          // `onClick('a')=b` is one attribute.
          flushArgs();
          ensureAttrName(part.startIndex);
          const [vs, ve] = innerValue(part, "attr_value_expr");
          addValue(
            part.type === "attr_bound_value" ? "attrValue:bound" : "attrValue",
            part.startIndex,
            part.endIndex,
            vs,
            ve,
          );
          cur = null;
          break;
        }
        case "attr_spread": {
          flushAttr();
          const [vs, ve] = innerValue(part, "attr_value_expr");
          addValue("attrSpread", part.startIndex, part.endIndex, vs, ve);
          cur = null;
          break;
        }
        case "attr_group":
          for (const inner of childrenOf(part)) visitTagPart(inner);
          break;
      }
      if (
        part.type === "attr_name" ||
        part.type === "attr_value" ||
        part.type === "attr_bound_value" ||
        part.type === "attr_spread" ||
        part.type === "method_body"
      ) {
        tagPartsSeenAttr = true;
      }
      curEnd = part.endIndex;
    };

    let tagPartsSeenAttr = false;

    for (const child of kids) {
      switch (child.type) {
        case "open_tag_start":
          add("openTagStart", child.startIndex, child.endIndex);
          break;
        case "tag_name":
          tagNameNode = child;
          addTemplate(
            "tagName",
            child.startIndex,
            child.endIndex,
            templateParts(child),
          );
          break;
        case "shorthand_id":
        case "shorthand_class":
          addTemplate(
            child.type === "shorthand_id"
              ? "tagShorthandId"
              : "tagShorthandClass",
            child.startIndex,
            child.endIndex,
            templateParts(child),
            child.startIndex + 1,
          );
          break;
        case "statement_expr":
          break; // no events
        case "tag_var": {
          const [vs, ve] = innerValue(child, "tag_var_expr");
          addValue("tagVar", child.startIndex, child.endIndex, vs, ve);
          break;
        }
        case "open_tag_end":
          flushAttr();
          openEnd = child;
          add("openTagEnd", child.startIndex, child.endIndex);
          break;
        case "open_tag_end_self":
          flushAttr();
          openEnd = child;
          add("openTagEnd:selfClosed", child.startIndex, child.endIndex);
          break;
        case "concise_open_tag_end":
          flushAttr();
          openEnd = child;
          add("openTagEnd", child.endIndex, child.endIndex);
          break;
        case "close_tag":
          closeTag = child;
          break;
        case "element_end":
          break;
        // attribute-machine parts (order matters)
        case "args":
        case "params":
        case "type_args":
        case "type_params":
        case "attr_group":
        case "attr_name":
        case "attr_value":
        case "attr_bound_value":
        case "attr_spread":
        case "method_body":
          visitTagPart(child);
          break;
        default:
          visit(child); // element children (text, nested elements, ...)
          break;
      }
    }

    const tagNameText = tagNameNode
      ? src.slice(tagNameNode.startIndex, tagNameNode.endIndex)
      : "";

    if (closeTag) {
      const ckids = childrenOf(closeTag);
      const cstart = ckids.find((c) => c.type === "close_tag_start");
      const cname = ckids.find((c) => c.type === "close_tag_name");
      const cend = ckids.find((c) => c.type === "close_tag_end");
      if (cstart) add("closeTagStart", cstart.startIndex, cstart.endIndex);
      if (cname) add("closeTagName", cname.startIndex, cname.endIndex);
      if (cend)
        add(`closeTagEnd(${tagNameText})`, cend.startIndex, cend.endIndex);
    } else if (openEnd) {
      const selfClosed = openEnd.type === "open_tag_end_self";
      const concise = openEnd.type === "concise_open_tag_end";
      const hasInterp =
        tagNameNode != null && templateParts(tagNameNode).length > 0;
      const name = hasInterp ? "" : tagNameText;
      const isVoid = !hasInterp && VOID_TAGS.has(name);
      const isStatement = !hasInterp && STATEMENT_TAGS.has(name);
      const endNode = kids.find((c) => c.type === "element_end");
      if (
        !selfClosed &&
        !isVoid &&
        !isStatement &&
        concise &&
        endNode &&
        !endNode.isMissing
      ) {
        const pos = implicitClosePos(endNode.startIndex);
        add(`closeTagEnd(${tagNameText})`, pos, pos);
      }
    }
  };

  const visit = (node: SyntaxNode) => {
    switch (node.type) {
      case "text":
        add("text", node.startIndex, node.endIndex);
        break;
      case "escape": {
        // A backslash run before ${ / $!{ (checkForPlaceholder): half the
        // backslashes are omitted from output; for odd runs the ${ is
        // literal. The text part adjoins the surrounding text events (the
        // merge normalization joins them).
        const n = node.endIndex - node.startIndex;
        if (n % 2) {
          // omitted = (n + 1) / 2; the rest joins the following text
          const start = node.startIndex + (n + 1) / 2;
          if (start < node.endIndex) add("text", start, node.endIndex);
        } else {
          // first half joins the preceding text; second half is omitted
          add("text", node.startIndex, node.startIndex + n / 2);
        }
        break;
      }
      case "placeholder":
        visitPlaceholder(node);
        break;
      case "html_comment":
        addValue(
          "comment",
          node.startIndex,
          node.endIndex,
          node.startIndex + 4,
          node.endIndex - 3,
        );
        break;
      case "line_comment":
        addValue(
          "comment",
          node.startIndex,
          node.endIndex,
          node.startIndex + 2,
          node.endIndex,
        );
        break;
      case "block_comment":
        addValue(
          "comment",
          node.startIndex,
          node.endIndex,
          node.startIndex + 2,
          node.endIndex - 2,
        );
        break;
      case "cdata":
        addValue(
          "cdata",
          node.startIndex,
          node.endIndex,
          node.startIndex + 9,
          node.endIndex - 3,
        );
        break;
      case "doctype":
        addValue(
          "doctype",
          node.startIndex,
          node.endIndex,
          node.startIndex + 2,
          node.endIndex - 1,
        );
        break;
      case "declaration": {
        const closeLen =
          src.charCodeAt(node.endIndex - 2) === 63 /* ? */ ? 2 : 1;
        addValue(
          "declaration",
          node.startIndex,
          node.endIndex,
          node.startIndex + 2,
          node.endIndex - closeLen,
        );
        break;
      }
      case "scriptlet": {
        const kids = childrenOf(node);
        const startTok = kids[0];
        const concise = startTok.type === "scriptlet_start_concise";
        const start = node.startIndex + (concise ? 1 : 0);
        const body = kids[1];
        if (body?.type === "scriptlet_block") {
          // The parser emits onScriptlet only when the block's "}" is
          // consumed; a MISSING close token means error recovery completed
          // the block where the parser errored out.
          const close = childrenOf(body).find(
            (c) => c.type === "scriptlet_block_close",
          );
          if (close?.isMissing) break;
          const [vs, ve] = innerValue(body, "scriptlet_block_expr");
          addValue("scriptlet:block", start, node.endIndex, vs, ve);
        } else if (body) {
          addValue(
            "scriptlet",
            start,
            node.endIndex,
            body.startIndex,
            body.endIndex,
          );
        }
        break;
      }
      case "element":
        visitElement(node);
        break;
      case "html_block":
      case "document":
        for (const child of childrenOf(node)) visit(child);
        break;
      // Stray nodes inside ERROR containers (error-case fixtures): map them
      // best-effort so the events before the parser's error still match.
      case "open_tag_start":
        add("openTagStart", node.startIndex, node.endIndex);
        break;
      case "tag_name":
      case "tag_name_fragment":
        addTemplate(
          "tagName",
          node.startIndex,
          node.endIndex,
          node.type === "tag_name" ? templateParts(node) : [],
        );
        break;
      case "shorthand_id":
        addTemplate(
          "tagShorthandId",
          node.startIndex,
          node.endIndex,
          templateParts(node),
          node.startIndex + 1,
        );
        break;
      case "shorthand_class":
        addTemplate(
          "tagShorthandClass",
          node.startIndex,
          node.endIndex,
          templateParts(node),
          node.startIndex + 1,
        );
        break;
      case "attr_name":
        add("attrName", node.startIndex, node.endIndex);
        break;
      case "attr_value":
      case "attr_bound_value": {
        // ensureAttrName: an empty name fires unless the attribute already
        // has one (attr_name directly before, possibly through its args).
        let prev = node.previousNamedSibling;
        if (prev?.type === "args") prev = prev.previousNamedSibling;
        if (
          prev?.type !== "attr_name" ||
          src.slice(prev.endIndex, node.startIndex).includes(",")
        ) {
          add("attrName", node.startIndex, node.startIndex);
        }
        const [vs, ve] = innerValue(node, "attr_value_expr");
        addValue(
          node.type === "attr_bound_value" ? "attrValue:bound" : "attrValue",
          node.startIndex,
          node.endIndex,
          vs,
          ve,
        );
        break;
      }
      case "attr_spread": {
        const [vs, ve] = innerValue(node, "attr_value_expr");
        addValue("attrSpread", node.startIndex, node.endIndex, vs, ve);
        break;
      }
      case "tag_var": {
        const [vs, ve] = innerValue(node, "tag_var_expr");
        addValue("tagVar", node.startIndex, node.endIndex, vs, ve);
        break;
      }
      case "args": {
        if (node.nextNamedSibling?.type === "method_body") break;
        const [vs, ve] = innerValue(node, "args_expr");
        const prev = node.previousNamedSibling;
        const attrish =
          prev?.type === "attr_name" ||
          prev?.type === "attr_value" ||
          prev?.type === "attr_bound_value" ||
          prev?.type === "attr_spread";
        const named =
          prev?.type === "attr_name" &&
          !src.slice(prev.endIndex, node.startIndex).includes(",");
        if (attrish && !named) {
          add("attrName", node.startIndex, node.startIndex);
        }
        addValue(
          attrish ? "attrArgs" : "tagArgs",
          node.startIndex,
          node.endIndex,
          vs,
          ve,
        );
        break;
      }
      case "attr_eq":
      case "attr_bound_eq": {
        // a stray = / := (its value failed to parse): ensureAttrName
        const prev = node.previousNamedSibling;
        if (
          prev?.type !== "attr_name" ||
          src.slice(prev.endIndex, node.startIndex).includes(",")
        ) {
          add("attrName", node.startIndex, node.startIndex);
        }
        break;
      }
      case "method_body": {
        // stray attr method: reconstruct from preceding siblings
        let args = node.previousNamedSibling;
        if (args?.type !== "args") break;
        let typeParams =
          args.previousNamedSibling?.type === "type_params"
            ? args.previousNamedSibling
            : undefined;
        const before = (typeParams ?? args).previousNamedSibling;
        const start = (typeParams ?? args).startIndex;
        if (
          before?.type !== "attr_name" ||
          src.slice(before.endIndex, start).includes(",")
        ) {
          add("attrName", start, start);
        }
        add("attrMethod", start, node.endIndex);
        if (typeParams) {
          const [vs, ve] = innerValue(typeParams, "type_expr");
          addValue(
            "attrMethod.typeParams",
            typeParams.startIndex,
            typeParams.endIndex,
            vs,
            ve,
          );
        }
        {
          const [vs, ve] = innerValue(args, "args_expr");
          addValue("attrMethod.params", args.startIndex, args.endIndex, vs, ve);
        }
        {
          const [bs, be] = innerValue(node, "method_body_expr");
          addValue("attrMethod.body", node.startIndex, node.endIndex, bs, be);
        }
        break;
      }
      case "params": {
        const [vs, ve] = innerValue(node, "params_expr");
        addValue("tagParams", node.startIndex, node.endIndex, vs, ve);
        break;
      }
      case "open_tag_end":
        add("openTagEnd", node.startIndex, node.endIndex);
        break;
      case "open_tag_end_self":
        add("openTagEnd:selfClosed", node.startIndex, node.endIndex);
        break;
      case "concise_open_tag_end":
        add("openTagEnd", node.endIndex, node.endIndex);
        break;
      case "close_tag_start":
        add("closeTagStart", node.startIndex, node.endIndex);
        break;
      case "close_tag_name":
        add("closeTagName", node.startIndex, node.endIndex);
        break;
      case "type_args": {
        const [vs, ve] = innerValue(node, "type_expr");
        addValue("tagTypeArgs", node.startIndex, node.endIndex, vs, ve);
        break;
      }
      default:
        // ERROR / MISSING / other containers: recurse to salvage events.
        for (const child of childrenOf(node)) visit(child);
        break;
    }
  };

  visit(root);
  return events;
}

export function sortEvents(events: Event[]): Event[] {
  return [...events].sort(
    (a, b) =>
      a.start - b.start || a.end - b.end || a.label.localeCompare(b.label),
  );
}
