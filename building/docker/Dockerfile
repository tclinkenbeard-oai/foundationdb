# Lock the version of rocky linux to one that is the same as the base image that we use for the
# backup agent
FROM openaiapibase.azurecr.io/mirror/foundationdb/rockylinux9-latest@sha256:27e84fcd4a82e63b1f499cf199c84fe9a1831c8858947b3d9e0434b22317075a AS base


ARG ENABLE_PYTHON_AC=0
ARG SCCACHE_VERSION=0.15.0

ENV ENABLE_PYTHON_AC=${ENABLE_PYTHON_AC}

RUN curl -fsSL \
      https://github.com/mozilla/sccache/releases/download/v${SCCACHE_VERSION}/sccache-v${SCCACHE_VERSION}-x86_64-unknown-linux-musl.tar.gz \
      -o /tmp/sccache.tar.gz && \
    tar -xzf /tmp/sccache.tar.gz --strip-components=1 -C /usr/local/bin && \
    rm -f /tmp/sccache.tar.gz

COPY building/docker/patches /patches
RUN curl -L https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz -o /tmp/zstd-1.5.7.tar.gz && \
    tar -xvf /tmp/zstd-1.5.7.tar.gz -C /tmp && \
    cd /tmp/zstd-1.5.7 && \
    patch -p1 < /patches/zstd-1.5.7-fdb-001.patch && \
    make cmakebuild


FROM base AS buildfdb
# Note we put the build output inside the mounted source because otherwise I/O perf are terrible it
# seems but we will need to store it somewhere at the end of this RUN otherwise it will be lost ...
RUN --mount=type=bind,target=/src/fdb,rw \
    --mount=type=cache,target=/root/.cache/sccache \
    --mount=type=secret,id=sccache_env \
    mkdir -p /src/fdb/build_output && \
    rm -rf /src/fdb/build_output/* && \
    cd /src/fdb && \
    while IFS='=' read -r key value; do \
        [ -n "${key}" ] && export "${key}=${value}"; \
    done < /run/secrets/sccache_env && \
    SCCACHE_DIR=/root/.cache/sccache \
    CC=clang CXX=clang++ LD=lld cmake \
        -D CMAKE_C_COMPILER_LAUNCHER=sccache \
        -D CMAKE_CXX_COMPILER_LAUNCHER=sccache \
        -D USE_LD=LLD -D USE_LIBCXX=1 -D USE_PYTHON_AC=$ENABLE_PYTHON_AC \
        -D BUILD_MAKO=ON \
        -D FULL_DEBUG_SYMBOLS=ON \
        -D CMAKE_EXPORT_COMPILE_COMMANDS=ON \
        -G Ninja -B build_output -S . && \
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
    ( SCCACHE_DIR=/root/.cache/sccache sccache --show-stats || true ) && \
    mkdir -p /out && \
    cp -a /src/fdb/build_output/packages /out/

FROM scratch AS artifacts
COPY --from=buildfdb /out/packages /packages
