#!/usr/bin/env bash
# Claude Code PostToolUse hook (matcher: Write|Edit|MultiEdit).
#
# When an edit touches a file that DEFINES the SD-card schema (firmware headers)
# or must MIRROR it (docs/software/CONFIG_IMPORT.md, the config-builder modules), run the
# tool's `node --test` suite — which includes test/schema-sync.test.js, the
# drift guard that asserts validate.js still matches the firmware buffer sizes
# and caps. On failure, exit 2 so the message is fed back to Claude to reconcile
# firmware <-> contract <-> tool in the same turn.
#
# See tools/config-builder/CLAUDE.md ("Keeping in sync") for the change order.
set -uo pipefail

root="${CLAUDE_PROJECT_DIR:-$(cd "$(dirname "$0")/../../.." && pwd)}"

# Pull the edited file path from the hook payload on stdin.
payload="$(cat)"
file="$(printf '%s' "$payload" \
  | python3 -c 'import json,sys; print(json.load(sys.stdin).get("tool_input",{}).get("file_path",""))' \
  2>/dev/null || true)"

# Only act on schema-relevant files; everything else is a no-op.
if ! printf '%s\n' "$file" | grep -Eq \
  '(include/app/(roster|teacher)\.h|include/services/config_service\.h|src/services/(roster|config)_service\.cpp|docs/software/CONFIG_IMPORT\.md|tools/config-builder/src/(validate|model|diario)\.js)$'; then
  exit 0
fi

if ! out="$(cd "$root/tools/config-builder" && node --test 2>&1)"; then
  {
    echo "SCHEMA DRIFT — a schema file changed and tools/config-builder tests fail."
    echo "Reconcile the firmware headers, docs/software/CONFIG_IMPORT.md, and the tool"
    echo "(validate.js LIMITS / model.js / diario.js), then rerun 'node --test'."
    echo "----- node --test (tail) -----"
    printf '%s\n' "$out" | tail -25
  } >&2
  exit 2
fi
exit 0
