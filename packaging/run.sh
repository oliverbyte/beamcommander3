#!/usr/bin/env bash
# Launcher for a packaged BeamCommander3 release (built by
# .github/workflows/release-macos.yml). The daemon serves the bundled
# frontend_dist itself - just run this and open the printed URL.
set -euo pipefail
cd "$(cd "$(dirname "$0")" && pwd)"
echo "Starting BeamCommander3 - open http://localhost:8000 once it's running."
exec ./laser_daemon "$@"
