#!/usr/bin/env bash
set -euo pipefail

dest_root="https://appliedciblobdata.blob.core.windows.net/fdb-ci-artifacts"
readonly default_joshua_proxy_addr="joshua-proxy-joshua-proxy.gateway.turtle-0s.internal.api.openai.org:443"
readonly default_joshua_runs="1000"
readonly default_joshua_smoke_timeout_seconds="10"
readonly default_cache_scope="global"
readonly default_cache_namespace="foundationdb"


if ! command -v buildkite-agent >/dev/null; then
  echo "buildkite-agent is required for bkbuildentrypoint.sh" >&2
  exit 1
fi

if ! command -v python3 >/dev/null; then
  echo "python3 is required for bkbuildentrypoint.sh" >&2
  exit 1
fi


resolve_dest() {
  if [[ -n "${BUILDKITE_PIPELINE_ID:-}" && -n "${BUILDKITE_BUILD_ID:-}" && -n "${BUILDKITE_JOB_ID:-}" ]]; then
    echo "${dest_root}/${BUILDKITE_PIPELINE_ID}/${BUILDKITE_BUILD_ID}/${BUILDKITE_JOB_ID}"
  else
    echo "${dest_root}"
  fi
}

sanitize_path_component() {
  local value="${1:-unknown}"
  # Use a locale-stable character class; keep '-' last so it is treated literally.
  value="$(printf '%s' "${value}" | LC_ALL=C tr '/:@ ' '_' | LC_ALL=C tr -c 'A-Za-z0-9._-' '_')"
  if [[ -z "${value}" ]]; then
    echo "unknown"
  else
    echo "${value}"
  fi
}

resolve_cache_dest() {
  local cache_scope
  local cache_namespace
  local arch
  local python_ac

  cache_scope="$(sanitize_path_component "${BUILDKITE_CACHE_SCOPE:-${default_cache_scope}}")"
  cache_namespace="$(sanitize_path_component "${BUILDKITE_CACHE_NAMESPACE:-${default_cache_namespace}}")"
  arch="$(sanitize_path_component "$(uname -m)")"
  python_ac="$(sanitize_path_component "${ENABLE_PYTHON_AC:-0}")"
  echo "${dest_root}/cache/${cache_namespace}/${cache_scope}/${arch}/pyac-${python_ac}"
}

buildx_cache_file_name() {
  local cache_version
  cache_version="$(sanitize_path_component "${BUILDX_CACHE_VERSION:-v1}")"
  if tar --help 2>/dev/null | grep -q -- '--zstd'; then
    echo "buildx-cache-${cache_version}.tar.zst"
  else
    echo "buildx-cache-${cache_version}.tar.gz"
  fi
}

dir_has_files() {
  local dir="${1}"
  local found=""
  if [[ ! -d "${dir}" ]]; then
    return 1
  fi
  found="$(find "${dir}" -mindepth 1 -print -quit 2>/dev/null || true)"
  [[ -n "${found}" ]]
}

restore_buildx_cache_from_url() {
  python3 building/scripts/restore_buildx_cache_from_url.py "${1}"
}

restore_buildx_cache() {
  local cache_file
  local cache_dest
  local cache_url

  if ! command -v curl >/dev/null; then
    echo "curl not found; skipping remote build cache restore"
    return
  fi

  cache_file="$(buildx_cache_file_name)"
  cache_dest="$(resolve_cache_dest)"
  cache_url="${cache_dest}/${cache_file}"

  echo "--- Build cache restore"
  echo "~~~ Trying shared cache"
  echo "Restoring buildx cache from ${cache_url}"
  if restore_buildx_cache_from_url "${cache_url}"; then
    echo "Restored buildx cache from ${cache_url}"
    return
  fi

  echo "No remote buildx cache found; continuing with cold cache"
}

save_buildx_cache() {
  local cache_file
  local cache_dest

  cache_file="$(buildx_cache_file_name)"
  cache_dest="$(resolve_cache_dest)"
  python3 building/scripts/save_buildx_cache.py \
    --cache-file "${cache_file}" \
    --cache-dest "${cache_dest}"
}

collect_changed_cpp_header_files() {
  local diff_range=""
  local base_branch="${BUILDKITE_PULL_REQUEST_BASE_BRANCH:-}"
  local base_ref=""
  local merge_base=""

  if [[ "${BUILDKITE_PULL_REQUEST:-false}" != "false" && -n "${base_branch}" ]]; then
    base_ref="origin/${base_branch}"
    if ! git rev-parse --verify --quiet "${base_ref}" >/dev/null; then
      git fetch --quiet origin "${base_branch}" || true
    fi
    if git rev-parse --verify --quiet "${base_ref}" >/dev/null; then
      if merge_base="$(git merge-base HEAD "${base_ref}")"; then
        diff_range="${merge_base}...HEAD"
      fi
    fi
  fi

  if [[ -z "${diff_range}" ]]; then
    if git rev-parse --verify --quiet HEAD~1 >/dev/null; then
      diff_range="HEAD~1..HEAD"
    else
      diff_range="$(git hash-object -t tree /dev/null)..HEAD"
    fi
  fi

  git diff --name-only --diff-filter=ACMR "${diff_range}" -- '*.h' '*.cpp'
}

run_clang_format_check_in_docker() {
  local changed_files
  local clang_format_image

  mapfile -t changed_files < <(
    collect_changed_cpp_header_files | while IFS= read -r path; do
      [[ -f "${path}" ]] && printf '%s\n' "${path}"
    done
  )

  if (( ${#changed_files[@]} == 0 )); then
    echo "No changed *.h or *.cpp files found; skipping clang-format"
    return 0
  fi

  if ! command -v docker >/dev/null; then
    echo "docker is required to run clang-format check in container" >&2
    return 1
  fi

  clang_format_image="${CLANG_FORMAT_IMAGE:-foundationdb/devel:rockylinux9-latest}"
  echo "Checking clang-format for ${#changed_files[@]} changed *.h/*.cpp files in ${clang_format_image}"
  docker run --rm \
    -v "$(pwd):/workspace" \
    -w /workspace \
    "${clang_format_image}" \
    clang-format --dry-run -Werror "${changed_files[@]}"
}

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

echo "--- Joshua Proxy preflight"
joshua_proxy_addr="${JOSHUA_PROXY_ADDR:-${default_joshua_proxy_addr}}"
joshua_smoke_timeout_seconds="${JOSHUA_SMOKE_TIMEOUT_SECONDS:-${default_joshua_smoke_timeout_seconds}}"
if ! [[ "${joshua_smoke_timeout_seconds}" =~ ^[1-9][0-9]*$ ]]; then
  echo "JOSHUA_SMOKE_TIMEOUT_SECONDS must be a positive integer, got: ${joshua_smoke_timeout_seconds}" >&2
  exit 1
fi
echo "~~~ Checking Joshua Proxy reachability"
echo "Smoke testing ${joshua_proxy_addr} before build"
smoke_args=(
  python3 building/joshua_proxy/joshua_proxy_client.py
  smoke
  --addr "${joshua_proxy_addr}"
  --timeout-seconds "${joshua_smoke_timeout_seconds}"
)
if [[ "${JOSHUA_PROXY_INSECURE:-}" == "1" ]]; then
  smoke_args+=(--insecure)
fi
if ! "${smoke_args[@]}"; then
  exit 1
fi

echo "--- clang-format"
if ! command -v git >/dev/null; then
  echo "git not found; skipping clang-format on changed files"
else
  run_clang_format_check_in_docker
fi

echo "--- Building artifacts"
restore_buildx_cache
mkdir -p .ci-cache
rm -rf .ci-cache/buildx-new
buildx_cache_from_args=()
if dir_has_files ".ci-cache/buildx"; then
  buildx_cache_from_args+=(--cache-from "type=local,src=.ci-cache/buildx")
fi
docker buildx build \
  --target artifacts \
  "${buildx_cache_from_args[@]}" \
  --cache-to type=local,dest=.ci-cache/buildx-new,mode=max \
  --output type=local,dest=build_output \
  -f building/docker/Dockerfile .
save_buildx_cache

tarball_dir="build_output/packages"
shopt -s nullglob
tarballs=("${tarball_dir}/correctness"*.tar.gz)
shopt -u nullglob

if (( ${#tarballs[@]} )); then
  mapfile -t tarballs < <(printf '%s\n' "${tarballs[@]}" | sort)
  selected_tarball="${tarballs[0]}"
  selected_tarball_basename="$(basename "${selected_tarball}")"
  echo "~~~ Tarball selection"
  if (( ${#tarballs[@]} > 1 )); then
    echo "Found ${#tarballs[@]} correctness tarballs; submitting only ${selected_tarball_basename}"
  else
    echo "Found one correctness tarball: ${selected_tarball_basename}"
  fi

  echo "--- Uploading correctness packages"
  dest="$(resolve_dest)"
  selected_tarball_relative_path="${selected_tarball#./}"
  selected_tarball_url="${dest}/${selected_tarball_relative_path}"
  echo "~~~ Upload destination"
  echo "Uploading correctness packages to ${dest}"
  if ! BUILDKITE_ARTIFACT_UPLOAD_DESTINATION="${dest}" \
       buildkite-agent artifact upload "${tarballs[@]}"; then
    echo "Failed to upload correctness packages" >&2
    exit 1
  fi
  echo "~~~ Submitting Joshua job"
  joshua_proxy_addr="${JOSHUA_PROXY_ADDR:-${default_joshua_proxy_addr}}"
  joshua_runs="${JOSHUA_RUNS:-${default_joshua_runs}}"
  if ! [[ "${joshua_runs}" =~ ^[1-9][0-9]*$ ]]; then
    echo "JOSHUA_RUNS must be a positive integer, got: ${joshua_runs}" >&2
    exit 1
  fi
  echo "Submitting tarball URL ${selected_tarball_url} to ${joshua_proxy_addr} with runs=${joshua_runs}"
  submit_args=(
    python3 building/joshua_proxy/joshua_proxy_client.py
    submit
    --addr "${joshua_proxy_addr}"
    --correctness-package-url "${selected_tarball_url}"
    --runs "${joshua_runs}"
  )
  if [[ "${JOSHUA_PROXY_INSECURE:-}" == "1" ]]; then
    submit_args+=(--insecure)
  fi
  if ! joshua_job_id="$("${submit_args[@]}")"; then
    echo "Failed to submit Joshua job" >&2
    exit 1
  fi
  if [[ -z "${joshua_job_id}" ]]; then
    echo "SubmitJob response did not include job_id/jobId" >&2
    exit 1
  fi
  echo "Submitted Joshua job_id=${joshua_job_id}"
else
  echo "No correctness tarballs found under ${tarball_dir}"
  exit 1
fi
