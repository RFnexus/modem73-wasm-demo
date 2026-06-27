#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PORT="${1:-8080}"
cd "$HERE/web"
echo "Serving $HERE/web at http://localhost:$PORT/"
exec python3 -m http.server "$PORT"
