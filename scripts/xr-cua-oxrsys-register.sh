#!/usr/bin/env bash
# Register the xr-cua-oxrsys MCP server in ~/.claude.json for Claude Code sessions.
# Idempotent: writes the row once, keeps every other server, and prints the
# before/after diff so the operator sees what changed.
set -euo pipefail
CFG="${CLAUDE_CONFIG:-$HOME/.claude.json}"
if [ ! -f "$CFG" ]; then
  echo "no $CFG; run Claude Code once to create it, then re-run this script" >&2
  exit 1
fi
if ! command -v cua-driver >/dev/null 2>&1; then
  echo "note: cua-driver is not on PATH; the row is registered anyway" >&2
fi
BAK="$CFG.$(date +%s).bak"
cp "$CFG" "$BAK"
python3 - "$CFG" <<'PY'
import json, pathlib, sys
p = pathlib.Path(sys.argv[1])
data = json.loads(p.read_text())
servers = data.setdefault("mcpServers", {})
want = {"command": "cua-driver", "args": ["mcp"]}
if servers.get("xr-cua-oxrsys") == want:
    sys.exit(0)
servers["xr-cua-oxrsys"] = want
p.write_text(json.dumps(data, indent=2) + "\n")
PY
diff -u "$BAK" "$CFG" || true
echo "wrote xr-cua-oxrsys to $CFG; backup at $BAK"
