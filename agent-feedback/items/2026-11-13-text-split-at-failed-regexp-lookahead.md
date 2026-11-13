---
type: bug
impact: low
effort: high
site: src/scanner.c › regexp_rest_len
---

# Stop a failed regular expression lookahead from splitting text tokens

In a text body, a `/` that may start a regular expression peeks to the end of its line for the closing `/`. When none is found the peeked chars stay queued, and the EStream only marks a token end once its queue drains, so a placeholder where the lookahead stopped (eg inside a string) ends the text token back at the `/`. If that string is unterminated, the parser drops its pending text at the error but the tree keeps the piece before the split, so the events before the error differ. A fix needs token ends that do not wait for the lookahead queue to drain.

Check: `pnpm exec tsx tools/check.mts '<script>/"${x}</script>'` reports `+ text [8,9]`.
