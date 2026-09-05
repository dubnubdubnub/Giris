#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Isaac Chiu
# WebHID requires a secure context. http://localhost counts, so this is enough —
# no TLS, no build step.
set -euo pipefail
cd "$(dirname "$0")"
PORT="${1:-8000}"
echo "Giris scope -> http://localhost:$PORT/  (Chrome or Edge; WebHID is Chromium-only)"
exec python3 -m http.server "$PORT" --bind 127.0.0.1
