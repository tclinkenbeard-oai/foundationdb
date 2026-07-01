# Built by Dockerfile.fdb-ci-base from the pinned Rocky Linux toolchain,
# sccache, clang-format, and patched zstd. The digest makes ordinary builds
# independent of mutable tags and avoids rebuilding these layers per job.
FROM openaiapibase.azurecr.io/mirror/foundationdb/fdb-ci-base:rockylinux9-sccache-0.15.0-zstd-1.5.7-fdb-001-clang-format-19.1.5@sha256:8d5cce290a721f89c656c790e5c984f573b6e0ee036005063c2adfe34c485fae AS base

ARG ENABLE_PYTHON_AC=0

ENV ENABLE_PYTHON_AC=${ENABLE_PYTHON_AC}

FROM base AS buildfdb
# Note we put the build output inside the mounted source because otherwise I/O perf are terrible it
# seems but we will need to store it somewhere at the end of this RUN otherwise it will be lost ...
RUN --mount=type=bind,target=/src/fdb,rw \
    --mount=type=secret,id=sccache_env,required=true \
    mkdir -p /src/fdb/build_output && \
    mkdir -p /src/fdb/.buildkite-artifacts && \
    rm -rf /src/fdb/build_output/* && \
    cd /src/fdb && \
    while IFS='=' read -r key value; do \
        [ -n "${key}" ] && export "${key}=${value}"; \
    done < /run/secrets/sccache_env && \
    test -n "${SCCACHE_AZURE_CONNECTION_STRING:-}" && \
    test -n "${SCCACHE_AZURE_BLOB_CONTAINER:-}" && \
    test -n "${SCCACHE_AZURE_KEY_PREFIX:-}" && \
    CC=clang CXX=clang++ LD=lld cmake \
        -D CMAKE_C_COMPILER_LAUNCHER=sccache \
        -D CMAKE_CXX_COMPILER_LAUNCHER=sccache \
        -D USE_LD=LLD -D USE_LIBCXX=1 -D USE_PYTHON_AC=$ENABLE_PYTHON_AC \
        -D BUILD_MAKO=ON \
        -D FULL_DEBUG_SYMBOLS=ON \
        -D CMAKE_EXPORT_COMPILE_COMMANDS=ON \
        -G Ninja -B build_output -S . && \
    grep -Eq '^CMAKE_C_COMPILER_LAUNCHER(:[^=]+)?=sccache$' build_output/CMakeCache.txt && \
    grep -Eq '^CMAKE_CXX_COMPILER_LAUNCHER(:[^=]+)?=sccache$' build_output/CMakeCache.txt && \
    cd /src/fdb/build_output && \
    ninja -j20 && \
    ninja -j20 strip_fdbserver strip_fdbbackup strip_fdbcli strip_fdbmonitor strip_fdb_c strip_mako && \
    ninja -j20 bindings/python/python_binding && \
    mkdir -p /src/fdb/build_output/packages/include/foundationdb && \
    install -m 0644 \
        /src/fdb/bindings/c/foundationdb/fdb_c.h \
        /src/fdb/build_output/bindings/c/foundationdb/fdb_c_apiversion.g.h \
        /src/fdb/build_output/bindings/c/foundationdb/fdb_c_options.g.h \
        /src/fdb/bindings/c/foundationdb/fdb_c_types.h \
        /src/fdb/build_output/packages/include/foundationdb/ && \
    sccache --show-stats | \
        tee /src/fdb/.buildkite-artifacts/sccache-stats.txt && \
    grep -Eiq 'Cache location.*(azblob|azure)' /src/fdb/.buildkite-artifacts/sccache-stats.txt && \
    mkdir -p /out && \
    cp -a /src/fdb/build_output/packages /out/

FROM scratch AS artifacts
COPY --from=buildfdb /out/packages /packages
