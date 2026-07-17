#!/usr/bin/env bash
set -euo pipefail

dest_root="https://appliedciblobdata.blob.core.windows.net/fdb-ci-artifacts"
release_dest_root="https://appliedciblobdata.blob.core.windows.net/fdb-release-artifacts"
readonly default_joshua_proxy_addr="joshua-proxy-joshua-proxy.gateway.turtle-0s.internal.api.openai.org:443"
readonly default_joshua_runs="10000"
readonly default_joshua_smoke_timeout_seconds="10"
readonly default_cache_scope="global"
readonly default_cache_namespace="foundationdb"
readonly default_storage_account="appliedciblobdata"
readonly default_blob_container="fdb-ci-artifacts"
readonly build_step_key="general-build"
readonly step_artifact_dir=".buildkite-artifacts"
readonly correctness_artifact="${step_artifact_dir}/correctness.tar.gz"
readonly sccache_stats_artifact="${step_artifact_dir}/sccache-stats.txt"


if ! command -v buildkite-agent >/dev/null; then
  echo "buildkite-agent is required for bkbuildentrypoint.sh" >&2
  exit 1
fi

if ! command -v python3 >/dev/null; then
  echo "python3 is required for bkbuildentrypoint.sh" >&2
  exit 1
fi

if ! command -v az >/dev/null; then
  echo "az is required for bkbuildentrypoint.sh" >&2
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

resolve_sccache_key_prefix() {
  local cache_scope
  local cache_namespace
  local arch
  local python_ac

  cache_scope="$(sanitize_path_component "${BUILDKITE_CACHE_SCOPE:-${default_cache_scope}}")"
  cache_namespace="$(sanitize_path_component "${BUILDKITE_CACHE_NAMESPACE:-${default_cache_namespace}}")"
  arch="$(sanitize_path_component "$(uname -m)")"
  python_ac="$(sanitize_path_component "${ENABLE_PYTHON_AC:-0}")"
  echo "sccache/${cache_namespace}/${cache_scope}/${arch}/pyac-${python_ac}"
}

create_sccache_env_file() {
  local storage_account="${SCCACHE_AZURE_STORAGE_ACCOUNT:-${default_storage_account}}"
  local blob_container="${SCCACHE_AZURE_BLOB_CONTAINER:-${default_blob_container}}"
  local expiry
  local sas_token
  local env_file

  expiry="$(date -u -d '+1 hour' '+%Y-%m-%dT%H:%MZ')"
  sas_token="$(
    az storage container generate-sas \
      --auth-mode login \
      --account-name "${storage_account}" \
      --name "${blob_container}" \
      --permissions rw \
      --expiry "${expiry}" \
      --as-user \
      -o tsv
  )"
  env_file="$(mktemp /tmp/fdb-sccache-env.XXXXXX)"
  printf 'SCCACHE_AZURE_CONNECTION_STRING=BlobEndpoint=https://%s.blob.core.windows.net;SharedAccessSignature=%s\nSCCACHE_AZURE_BLOB_CONTAINER=%s\nSCCACHE_AZURE_KEY_PREFIX=%s\n' \
    "${storage_account}" \
    "${sas_token}" \
    "${blob_container}" \
    "$(resolve_sccache_key_prefix)" \
    > "${env_file}"
  echo "${env_file}"
}

upload_release_artifacts() {
  local release_version="${FDB_RELEASE_BLOB_VERSION:-}"
  local release_arch
  local release_dest
  local package_dir="build_output/packages"
  local binary_dir="build_output/packages/bin"
  local binaries=(fdbserver fdbbackup fdbrestore backup_agent fdbcli fdbmonitor mako)
  local debug_symbols=(fdbserver.debug fdbbackup.debug fdbcli.debug fdbmonitor.debug mako.debug)
  local binary_artifacts=("${binaries[@]}" "${debug_symbols[@]}")
  local client_artifacts=(
    lib/libfdb_c.so
    include/foundationdb/fdb_c.h
    include/foundationdb/fdb_c_apiversion.g.h
    include/foundationdb/fdb_c_options.g.h
    include/foundationdb/fdb_c_types.h
  )
  local artifact

  if [[ -z "${release_version}" ]]; then
    echo "FDB_RELEASE_BLOB_VERSION not set; skipping release artifact upload"
    return 0
  fi

  release_version="$(sanitize_path_component "${release_version}")"
  release_arch="$(sanitize_path_component "${FDB_RELEASE_BLOB_ARCH:-$(uname -m)}")"
  release_dest="${release_dest_root}/${release_version}/${release_arch}"

  for artifact in "${binary_artifacts[@]}"; do
    if [[ ! -f "${binary_dir}/${artifact}" ]]; then
      echo "Missing release artifact ${binary_dir}/${artifact}" >&2
      return 1
    fi
  done

  for artifact in "${client_artifacts[@]}"; do
    if [[ ! -f "${package_dir}/${artifact}" ]]; then
      echo "Missing release artifact ${package_dir}/${artifact}" >&2
      return 1
    fi
  done

  echo "--- Uploading release artifacts"
  echo "Uploading release binaries, debug symbols, and C client artifacts to ${release_dest}"
  (
    cd "${binary_dir}"
    for artifact in "${binary_artifacts[@]}"; do
      BUILDKITE_ARTIFACT_UPLOAD_DESTINATION="${release_dest}" \
        buildkite-agent artifact upload "${artifact}"
    done
  )
  (
    cd "${package_dir}"
    for artifact in "${client_artifacts[@]}"; do
      BUILDKITE_ARTIFACT_UPLOAD_DESTINATION="${release_dest}" \
        buildkite-agent artifact upload "${artifact}"
    done
  )
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

  clang_format_image="${CLANG_FORMAT_IMAGE:-openaiapibase.azurecr.io/mirror/foundationdb/fdb-ci-base:rockylinux9-sccache-0.15.0-zstd-1.5.7-fdb-001-clang-format-19.1.5@sha256:8d5cce290a721f89c656c790e5c984f573b6e0ee036005063c2adfe34c485fae}"

  echo "~~~ Checking changed files"
  echo "Checking clang-format for ${#changed_files[@]} changed *.h/*.cpp files in ${clang_format_image}"
  docker run --rm \
    --platform linux/amd64 \
    -v "$(pwd):/workspace" \
    -w /workspace \
    "${clang_format_image}" \
    clang-format --dry-run -Werror "${changed_files[@]}"
}

prepare_step_artifacts() {
  local package_dir="build_output/packages"
  local tarballs=()
  local selected_tarball

  mkdir -p "${step_artifact_dir}"
  shopt -s nullglob
  tarballs=("${package_dir}/correctness"*.tar.gz)
  shopt -u nullglob
  if (( ${#tarballs[@]} == 0 )); then
    echo "No correctness tarballs found under ${package_dir}" >&2
    return 1
  fi

  mapfile -t tarballs < <(printf '%s\n' "${tarballs[@]}" | sort)
  selected_tarball="${tarballs[0]}"
  if (( ${#tarballs[@]} > 1 )); then
    echo "Found ${#tarballs[@]} correctness tarballs; handing off only $(basename "${selected_tarball}")"
  fi
  cp "${selected_tarball}" "${correctness_artifact}"

  echo "Prepared ${correctness_artifact} from $(basename "${selected_tarball}")"
}

download_build_artifact() {
  local artifact="${1}"

  echo "Downloading ${artifact} from ${build_step_key}"
  buildkite-agent artifact download "${artifact}" . --step "${build_step_key}"
}

run_mode="${1:-all}"
case "${run_mode}" in
  all|build|submit-joshua) ;;
  *)
    echo "Usage: $0 [build|submit-joshua]" >&2
    exit 2
    ;;
esac

if [[ "${run_mode}" == "submit-joshua" ]]; then
  download_build_artifact "${correctness_artifact}"
  mkdir -p build_output/packages
  cp "${correctness_artifact}" build_output/packages/correctness.tar.gz
fi

if [[ "${run_mode}" != "submit-joshua" ]]; then
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
echo "Checking Joshua Proxy reachability at ${joshua_proxy_addr}"
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
# Azure-backed sccache provides cross-pod object reuse. Do not restore or
# rewrite a global multi-gigabyte BuildKit archive for ordinary builds.
mkdir -p "${step_artifact_dir}"
rm -f "${sccache_stats_artifact}"
sccache_env_file="$(create_sccache_env_file)"
trap 'rm -f "${preflight_file}" "${sccache_env_file}"' EXIT
docker buildx build \
  --target artifacts \
  --secret id=sccache_env,src="${sccache_env_file}" \
  --output type=local,dest=build_output \
  -f building/docker/Dockerfile .
upload_release_artifacts

if [[ "${run_mode}" == "build" ]]; then
  echo "--- Preparing downstream artifacts"
  prepare_step_artifacts
  exit 0
fi

fi

tarball_dir="build_output/packages"
if [[ "${run_mode}" == "submit-joshua" ]]; then
  # The submit step downloads the correctness artifact under this stable name.
  # Ignore any versioned tarballs left behind in a reused Buildkite checkout.
  tarballs=("${tarball_dir}/correctness.tar.gz")
else
  shopt -s nullglob
  tarballs=("${tarball_dir}/correctness"*.tar.gz)
  shopt -u nullglob
fi

if (( ${#tarballs[@]} )) && [[ -f "${tarballs[0]}" ]]; then
  mapfile -t tarballs < <(printf '%s\n' "${tarballs[@]}" | sort)
  selected_tarball="${tarballs[0]}"
  selected_tarball_basename="$(basename "${selected_tarball}")"

  echo "--- Uploading correctness packages"
  if (( ${#tarballs[@]} > 1 )); then
    echo "Found ${#tarballs[@]} correctness tarballs; submitting only ${selected_tarball_basename}"
  else
    echo "Found one correctness tarball: ${selected_tarball_basename}"
  fi
  dest="$(resolve_dest)"
  selected_tarball_relative_path="${selected_tarball#./}"
  selected_tarball_url="${dest}/${selected_tarball_relative_path}"
  echo "Uploading correctness packages to ${dest}"
  if ! BUILDKITE_ARTIFACT_UPLOAD_DESTINATION="${dest}" \
       buildkite-agent artifact upload "${selected_tarball}"; then
    echo "Failed to upload correctness packages" >&2
    exit 1
  fi
  echo "--- Submitting Joshua job"
  joshua_proxy_addr="${JOSHUA_PROXY_ADDR:-${default_joshua_proxy_addr}}"
  joshua_runs="${JOSHUA_RUNS:-${default_joshua_runs}}"
  joshua_commit_hash="${BUILDKITE_COMMIT:-}"
  joshua_description="${JOSHUA_DESCRIPTION:-}"
  joshua_pull_request="${BUILDKITE_PULL_REQUEST:-}"
  joshua_branch_name="${BUILDKITE_BRANCH:-}"
  joshua_build_number="${BUILDKITE_BUILD_NUMBER:-}"
  if [[ -z "${joshua_branch_name}" ]] && command -v git >/dev/null; then
    joshua_branch_name="$(git rev-parse --abbrev-ref HEAD 2>/dev/null || true)"
    if [[ "${joshua_branch_name}" == "HEAD" ]]; then
      joshua_branch_name=""
    fi
  fi
  if [[ -z "${joshua_description}" ]]; then
    if [[ -z "${joshua_pull_request}" || "${joshua_pull_request}" == "false" ]]; then
      echo "BUILDKITE_PULL_REQUEST unavailable; Joshua description will not include PR number"
    fi
    if [[ -z "${joshua_branch_name}" ]]; then
      echo "BUILDKITE_BRANCH unavailable and git branch name could not be resolved; Joshua description will not include branch name"
    fi
    if [[ -z "${joshua_build_number}" ]]; then
      echo "BUILDKITE_BUILD_NUMBER unavailable; Joshua description will not include build number"
    fi
    if [[ -n "${joshua_pull_request}" && "${joshua_pull_request}" != "false" && -n "${joshua_branch_name}" ]]; then
      joshua_description="PR ${joshua_pull_request} branch ${joshua_branch_name} CI"
    elif [[ -n "${joshua_pull_request}" && "${joshua_pull_request}" != "false" ]]; then
      joshua_description="PR ${joshua_pull_request} CI"
    elif [[ -n "${joshua_branch_name}" ]]; then
      joshua_description="Branch ${joshua_branch_name} CI"
    fi
    if [[ -n "${joshua_description}" && -n "${joshua_build_number}" ]]; then
      joshua_description="${joshua_description} build ${joshua_build_number}"
    fi
  fi
  if [[ -z "${joshua_commit_hash}" ]] && command -v git >/dev/null; then
    joshua_commit_hash="$(git rev-parse HEAD 2>/dev/null || true)"
  fi
  if ! [[ "${joshua_runs}" =~ ^[1-9][0-9]*$ ]]; then
    echo "JOSHUA_RUNS must be a positive integer, got: ${joshua_runs}" >&2
    exit 1
  fi
  echo "Submitting tarball URL ${selected_tarball_url} to ${joshua_proxy_addr} with runs=${joshua_runs}"
  if [[ -n "${joshua_commit_hash}" ]]; then
    echo "Including commit hash in Joshua submission: ${joshua_commit_hash}"
  else
    echo "Commit hash unavailable; submitting Joshua job without commit hash metadata"
  fi
  if [[ -n "${joshua_description}" ]]; then
    echo "Including Joshua submission description metadata"
  fi
  submit_args=(
    python3 building/joshua_proxy/joshua_proxy_client.py
    submit
    --addr "${joshua_proxy_addr}"
    --correctness-package-url "${selected_tarball_url}"
    --runs "${joshua_runs}"
  )
  if [[ -n "${joshua_commit_hash}" ]]; then
    submit_args+=(--commit-hash "${joshua_commit_hash}")
  fi
  if [[ -n "${joshua_description}" ]]; then
    submit_args+=(--description "${joshua_description}")
  fi
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
