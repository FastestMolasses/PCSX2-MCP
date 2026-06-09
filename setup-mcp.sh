#!/usr/bin/env bash
#
# PCSX2-MCP setup script — macOS / Linux
# Mac/Unix equivalent of setup-mcp.bat.
#
# Builds the Node MCP server (if needed) and registers it with one or more
# AI agents: Claude Code (CLI), Claude Desktop, Cursor, and VS Code.
#
# Usage:
#   ./setup-mcp.sh                 # interactive — pick targets
#   ./setup-mcp.sh --all           # register with every detected target
#   ./setup-mcp.sh --claude-code --claude-desktop --cursor --vscode
#
# Optional: export PS2RECOMP_ROOT=/path/to/PS2Recomp before running to enable
# the ps2recomp_* tools (it gets baked into the written config's "env" block).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SERVER_DIR="$SCRIPT_DIR/pcsx2-mcp-server"
DIST="$SERVER_DIR/dist/index.js"
PS2RECOMP_ROOT="${PS2RECOMP_ROOT:-}"

green() { printf '\033[32m%s\033[0m\n' "$1"; }
yellow() { printf '\033[33m%s\033[0m\n' "$1"; }
red() { printf '\033[31m%s\033[0m\n' "$1"; }

# ---------------------------------------------------------------------------
# 1. Prerequisites: Node >= 18
# ---------------------------------------------------------------------------
if ! command -v node >/dev/null 2>&1; then
  red "Node.js is not installed. Install Node >= 18 from https://nodejs.org/ (or 'brew install node')."
  exit 1
fi
NODE_MAJOR="$(node -p 'process.versions.node.split(".")[0]')"
if [ "$NODE_MAJOR" -lt 18 ]; then
  red "Node.js >= 18 required (found $(node --version))."
  exit 1
fi
green "Node.js $(node --version) OK"

# ---------------------------------------------------------------------------
# 2. Build the MCP server if dist is missing
# ---------------------------------------------------------------------------
if [ ! -f "$DIST" ]; then
  yellow "Building MCP server (dist not found)..."
  npm --prefix "$SERVER_DIR" install
  npm --prefix "$SERVER_DIR" run build
fi
if [ ! -f "$DIST" ]; then
  red "Build failed — $DIST not found."
  exit 1
fi
green "MCP server built: $DIST"

# Build the "env" fragment node helpers will inject into JSON configs.
# Empty when PS2RECOMP_ROOT is unset.
export DIST PS2RECOMP_ROOT

# ---------------------------------------------------------------------------
# 3. Target selection
# ---------------------------------------------------------------------------
DO_CLAUDE_CODE=0; DO_CLAUDE_DESKTOP=0; DO_CURSOR=0; DO_VSCODE=0
if [ "$#" -eq 0 ]; then
  echo
  echo "Which agent(s) do you want to configure? (space-separated numbers, e.g. '1 2')"
  echo "  1) Claude Code (CLI)"
  echo "  2) Claude Desktop"
  echo "  3) Cursor"
  echo "  4) VS Code (project .vscode/mcp.json)"
  echo "  5) All"
  read -r -p "> " CHOICE
  for c in $CHOICE; do
    case "$c" in
      1) DO_CLAUDE_CODE=1 ;;
      2) DO_CLAUDE_DESKTOP=1 ;;
      3) DO_CURSOR=1 ;;
      4) DO_VSCODE=1 ;;
      5) DO_CLAUDE_CODE=1; DO_CLAUDE_DESKTOP=1; DO_CURSOR=1; DO_VSCODE=1 ;;
    esac
  done
else
  for arg in "$@"; do
    case "$arg" in
      --claude-code) DO_CLAUDE_CODE=1 ;;
      --claude-desktop) DO_CLAUDE_DESKTOP=1 ;;
      --cursor) DO_CURSOR=1 ;;
      --vscode) DO_VSCODE=1 ;;
      --all) DO_CLAUDE_CODE=1; DO_CLAUDE_DESKTOP=1; DO_CURSOR=1; DO_VSCODE=1 ;;
      *) red "Unknown option: $arg"; exit 1 ;;
    esac
  done
fi

# ---------------------------------------------------------------------------
# JSON merge helper — adds mcpServers.pcsx2 (or servers.pcsx2) to a config
# file, preserving any existing content. Args: <file> <topKey>
# ---------------------------------------------------------------------------
merge_json_config() {
  local file="$1" topKey="$2"
  mkdir -p "$(dirname "$file")"
  node - "$file" "$topKey" <<'NODE'
const fs = require('fs');
const [file, topKey] = process.argv.slice(2);
const dist = process.env.DIST;
const ps2 = process.env.PS2RECOMP_ROOT || '';
let cfg = {};
if (fs.existsSync(file)) {
  try { cfg = JSON.parse(fs.readFileSync(file, 'utf8')); }
  catch (e) { console.error('  Existing config is not valid JSON, aborting this target: ' + file); process.exit(2); }
}
cfg[topKey] = cfg[topKey] || {};
const entry = { command: 'node', args: [dist] };
if (ps2) entry.env = { PS2RECOMP_ROOT: ps2 };
cfg[topKey].pcsx2 = entry;
fs.writeFileSync(file, JSON.stringify(cfg, null, 2) + '\n');
console.log('  Wrote ' + file);
NODE
}

CONFIGURED=0

# ---------------------------------------------------------------------------
# 4a. Claude Code (CLI) — prefer the official `claude mcp add`
# ---------------------------------------------------------------------------
if [ "$DO_CLAUDE_CODE" -eq 1 ]; then
  echo; yellow "Configuring Claude Code (CLI)..."
  if command -v claude >/dev/null 2>&1; then
    ENV_ARGS=()
    [ -n "$PS2RECOMP_ROOT" ] && ENV_ARGS=(--env "PS2RECOMP_ROOT=$PS2RECOMP_ROOT")
    claude mcp remove pcsx2 >/dev/null 2>&1 || true
    claude mcp add pcsx2 --scope user "${ENV_ARGS[@]}" -- node "$DIST"
    green "  Registered with Claude Code (user scope)."
  else
    yellow "  'claude' CLI not found; writing ~/.claude.json directly."
    merge_json_config "$HOME/.claude.json" "mcpServers"
  fi
  CONFIGURED=1
fi

# ---------------------------------------------------------------------------
# 4b. Claude Desktop
# ---------------------------------------------------------------------------
if [ "$DO_CLAUDE_DESKTOP" -eq 1 ]; then
  echo; yellow "Configuring Claude Desktop..."
  if [ "$(uname)" = "Darwin" ]; then
    CD_CFG="$HOME/Library/Application Support/Claude/claude_desktop_config.json"
  else
    CD_CFG="$HOME/.config/Claude/claude_desktop_config.json"
  fi
  merge_json_config "$CD_CFG" "mcpServers"
  CONFIGURED=1
fi

# ---------------------------------------------------------------------------
# 4c. Cursor (global ~/.cursor/mcp.json)
# ---------------------------------------------------------------------------
if [ "$DO_CURSOR" -eq 1 ]; then
  echo; yellow "Configuring Cursor..."
  merge_json_config "$HOME/.cursor/mcp.json" "mcpServers"
  CONFIGURED=1
fi

# ---------------------------------------------------------------------------
# 4d. VS Code — project-local .vscode/mcp.json (uses "servers" key)
# ---------------------------------------------------------------------------
if [ "$DO_VSCODE" -eq 1 ]; then
  echo; yellow "Configuring VS Code (.vscode/mcp.json in current directory)..."
  merge_json_config "$(pwd)/.vscode/mcp.json" "servers"
  CONFIGURED=1
fi

echo
if [ "$CONFIGURED" -eq 1 ]; then
  green "Done. Restart your agent so it picks up the pcsx2 MCP server."
  [ -z "$PS2RECOMP_ROOT" ] && yellow "Tip: re-run with PS2RECOMP_ROOT=/path/to/PS2Recomp to enable the ps2recomp_* tools."
else
  yellow "No targets selected — nothing changed."
fi
