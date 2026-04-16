FROM foundationdb/build:rockylinux9-latest AS base

ARG ENABLE_PYTHON_AC=0

ENV ENABLE_PYTHON_AC=${ENABLE_PYTHON_AC}

RUN dnf -y install ccache && \
    dnf clean all

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
    --mount=type=cache,target=/root/.cache/ccache \
    mkdir -p /src/fdb/build_output && \
    rm -rf /src/fdb/build_output/* && \
    cd /src/fdb && \
    CCACHE_DIR=/root/.cache/ccache \
    CC=clang CXX=clang++ LD=lld cmake \
        -D CMAKE_C_COMPILER_LAUNCHER=ccache \
        -D CMAKE_CXX_COMPILER_LAUNCHER=ccache \
        -D USE_LD=LLD -D USE_LIBCXX=1 -D USE_PYTHON_AC=$ENABLE_PYTHON_AC \
        -D FULL_DEBUG_SYMBOLS=ON \
        -D CMAKE_EXPORT_COMPILE_COMMANDS=ON \
        -G Ninja -B build_output -S . && \
    cd /src/fdb/build_output && \
    ninja -j20 && \
    ninja -j20 bindings/python/python_binding && \
    ccache -s && \
    mkdir -p /out && \
    cp -a /src/fdb/build_output/packages /out/

FROM scratch AS artifacts
COPY --from=buildfdb /out/packages /packages
