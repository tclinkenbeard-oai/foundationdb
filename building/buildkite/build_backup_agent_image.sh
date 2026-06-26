#!/usr/bin/env bash
set -euo pipefail

exec bash building/buildkite/run_step.sh build-backup-agent-image
