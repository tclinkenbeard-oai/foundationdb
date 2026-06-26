#!/usr/bin/env bash
set -euo pipefail

echo "Doing a build from $(pwd)"
exec buildkite-agent pipeline upload building/buildkite/pipeline.yml
