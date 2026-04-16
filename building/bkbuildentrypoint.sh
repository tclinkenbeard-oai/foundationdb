#!/usr/bin/env bash
set -euo pipefail

dest_root="https://appliedciblobdata.blob.core.windows.net/fdb-ci-artifacts"
readonly default_joshua_proxy_addr="joshua-proxy-joshua-proxy.gateway.turtle-0s.internal.api.openai.org:443"
readonly default_joshua_runs="1000"
readonly default_joshua_smoke_timeout_seconds="10"
readonly default_cache_scope="global"
readonly default_cache_namespace="foundationdb"


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
  local cache_url="${1}"
  local archive_file

  archive_file="$(mktemp /tmp/buildx-cache.XXXXXX)"
  if ! curl -fsSL --retry 3 --retry-all-errors -o "${archive_file}" "${cache_url}"; then
    rm -f "${archive_file}"
    return 1
  fi

  rm -rf .ci-cache/buildx
  mkdir -p .ci-cache
  if [[ "${cache_url}" == *.tar.zst ]]; then
    if ! tar --zstd -xf "${archive_file}" -C .ci-cache; then
      rm -f "${archive_file}"
      rm -rf .ci-cache/buildx
      return 1
    fi
  elif [[ "${cache_url}" == *.tar.gz ]]; then
    if ! tar -xzf "${archive_file}" -C .ci-cache; then
      rm -f "${archive_file}"
      rm -rf .ci-cache/buildx
      return 1
    fi
  else
    rm -f "${archive_file}"
    rm -rf .ci-cache/buildx
    return 1
  fi
  rm -f "${archive_file}"

  dir_has_files ".ci-cache/buildx"
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

  if ! command -v buildkite-agent >/dev/null; then
    echo "buildkite-agent not found; skipping build cache upload"
    return
  fi
  if ! dir_has_files ".ci-cache/buildx-new"; then
    echo "No buildx cache output found under .ci-cache/buildx-new; skipping upload"
    return
  fi

  rm -rf .ci-cache/buildx
  mv .ci-cache/buildx-new .ci-cache/buildx
  cache_file="$(buildx_cache_file_name)"
  cache_dest="$(resolve_cache_dest)"

  mkdir -p .ci-cache
  rm -f ".ci-cache/${cache_file}"
  if [[ "${cache_file}" == *.tar.zst ]]; then
    if ! (
      cd .ci-cache
      tar --zstd -cf "${cache_file}" buildx
    ); then
      echo "Failed to package buildx cache with zstd; skipping upload" >&2
      return
    fi
  else
    if ! (
      cd .ci-cache
      tar -czf "${cache_file}" buildx
    ); then
      echo "Failed to package buildx cache with gzip; skipping upload" >&2
      return
    fi
  fi

  echo "--- Build cache upload"
  echo "~~~ Upload destination"
  echo "Uploading buildx cache to ${cache_dest}"
  if ! (
    cd .ci-cache
    BUILDKITE_ARTIFACT_UPLOAD_DESTINATION="${cache_dest}" \
      buildkite-agent artifact upload "${cache_file}"
  ); then
    echo "Failed to upload buildx cache; continuing without failing the build" >&2
  fi
}

install_grpcurl() {
  local grpcurl_version="${GRPCURL_VERSION:-1.9.3}"
  local arch
  local asset
  local tmp_tar
  local tmp_dir
  local install_dir

  arch="$(uname -m)"
  case "${arch}" in
    x86_64) asset="grpcurl_${grpcurl_version}_linux_x86_64.tar.gz" ;;
    aarch64) asset="grpcurl_${grpcurl_version}_linux_arm64.tar.gz" ;;
    *)
      echo "Unsupported architecture for grpcurl: ${arch}" >&2
      return 1
      ;;
  esac

  tmp_tar="$(mktemp /tmp/grpcurl.XXXXXX.tar.gz)"
  tmp_dir="$(mktemp -d /tmp/grpcurl.XXXXXX)"
  curl -fsSL -o "${tmp_tar}" \
    "https://github.com/fullstorydev/grpcurl/releases/download/v${grpcurl_version}/${asset}"

  if [[ -w /usr/local/bin ]]; then
    install_dir="/usr/local/bin"
  else
    install_dir="${HOME:-/tmp}/.local/bin"
    mkdir -p "${install_dir}"
    export PATH="${install_dir}:${PATH}"
  fi

  tar --no-same-owner --no-same-permissions -C "${tmp_dir}" -xzf "${tmp_tar}" grpcurl
  install -m 0755 "${tmp_dir}/grpcurl" "${install_dir}/grpcurl"
  rm -rf "${tmp_dir}"
  rm -f "${tmp_tar}"
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

if command -v buildkite-agent >/dev/null; then
  echo "--- Joshua Proxy preflight"
  if ! command -v python3 >/dev/null; then
    echo "python3 is required for Joshua Proxy integration but was not found in PATH" >&2
    exit 1
  fi
  if ! command -v grpcurl >/dev/null; then
    echo "grpcurl not found in PATH; installing"
    if ! install_grpcurl; then
      echo "Failed to install grpcurl" >&2
      exit 1
    fi
  fi
  if ! command -v grpcurl >/dev/null; then
    echo "grpcurl is required to submit to Joshua Proxy but was not found in PATH after install attempt" >&2
    exit 1
  fi
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

  if command -v buildkite-agent >/dev/null; then
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
    if ! command -v python3 >/dev/null; then
      echo "python3 is required for Joshua Proxy integration but was not found in PATH" >&2
      exit 1
    fi
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
    echo "buildkite-agent not found; skipping upload"
  fi
else
  echo "No correctness tarballs found under ${tarball_dir}"
  exit 1
fi
