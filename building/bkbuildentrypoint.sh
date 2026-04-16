#!/usr/bin/env bash
set -euo pipefail

dest_root="https://appliedciblobdata.blob.core.windows.net/fdb-ci-artifacts"

resolve_dest() {
  if [[ -n "${BUILDKITE_PIPELINE_ID:-}" && -n "${BUILDKITE_BUILD_ID:-}" && -n "${BUILDKITE_JOB_ID:-}" ]]; then
    echo "${dest_root}/${BUILDKITE_PIPELINE_ID}/${BUILDKITE_BUILD_ID}/${BUILDKITE_JOB_ID}"
  else
    echo "${dest_root}"
  fi
}

if command -v buildkite-agent >/dev/null; then
  echo "--- Blobstore preflight"
  dest="$(resolve_dest)"
  echo "~~~ Creating preflight artifact"
  preflight_file="$(mktemp /tmp/fdb-blobstore-preflight.XXXXXX)"
  trap 'rm -f "${preflight_file}"' EXIT
  printf 'blobstore preflight %s\n' "$(date -u +%FT%TZ)" > "${preflight_file}"
  echo "~~~ Validating artifact upload destination"
  echo "Validating artifact upload destination ${dest}"
  if ! BUILDKITE_ARTIFACT_UPLOAD_DESTINATION="${dest}" \
       buildkite-agent artifact upload "${preflight_file}"; then
    echo "Failed to validate artifact upload destination" >&2
    exit 1
  fi
else
  echo "buildkite-agent not found; skipping blobstore validation"
fi

echo "--- Building artifacts"
docker buildx build \
  --target artifacts \
  --output type=local,dest=build_output \
  -f building/docker/Dockerfile .

tarball_dir="build_output/packages"
shopt -s nullglob
tarballs=("${tarball_dir}/correctness"*.tar.gz)
shopt -u nullglob

if (( ${#tarballs[@]} )); then
  if command -v buildkite-agent >/dev/null; then
    echo "--- Uploading correctness packages"
    dest="$(resolve_dest)"
    echo "~~~ Upload destination"
    echo "Uploading correctness packages to ${dest}"
    if ! BUILDKITE_ARTIFACT_UPLOAD_DESTINATION="${dest}" \
         buildkite-agent artifact upload "${tarballs[@]}"; then
      echo "Failed to upload correctness packages" >&2
      exit 1
    fi
  else
    echo "buildkite-agent not found; skipping upload"
  fi
else
  echo "No correctness tarballs found under ${tarball_dir}"
  exit 1
fi
