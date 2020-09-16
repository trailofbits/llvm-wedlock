ARG BUILD_TYPE=Release
ARG LLVM_WEDLOCK_INSTALL_DIR=/opt/llvm-wedlock

FROM ubuntu:20.04 as base

FROM base as build
# Must explicitly list all global args within an image definition
ARG BUILD_TYPE
ARG LLVM_WEDLOCK_INSTALL_DIR

RUN apt-get update && \
    apt-get -y install clang-10 cmake zlib1g-dev ninja-build \
        python3.8 python3.8-dev python3-pip && \
    update-alternatives --install /usr/bin/cc cc /usr/bin/clang-10 100 && \
    update-alternatives --install /usr/bin/c++ c++ /usr/bin/clang++-10 100 && \
    update-alternatives --install /usr/bin/clang clang /usr/bin/clang-10 100 && \
    update-alternatives --install /usr/bin/clang++ clang++ /usr/bin/clang++-10 100 && \
    rm -rf /var/lib/apt/lists/*

# Build the Wedlock pass
WORKDIR /llvm-wedlock/build
COPY ./ /llvm-wedlock

RUN cmake \
    -DCMAKE_INSTALL_PREFIX:PATH=${LLVM_WEDLOCK_INSTALL_DIR} \
    -DCMAKE_BUILD_TYPE=${BUILD_TYPE} \
    -DLLVM_TARGETS_TO_BUILD="X86" \
    -DCLANG_ENABLE_ARCMT=OFF \
    -DLLVM_ENABLE_PROJECTS="clang;compiler-rt" \
    -G "Ninja" \
    ../llvm && cmake --build . --target install

# Start fresh on our base again
FROM base
ARG LLVM_WEDLOCK_INSTALL_DIR

# Get runtime dependencies of llvm-10
RUN apt-get update && \
    apt-get install -y $(apt-cache depends llvm-10 | grep Depends | sed "s/.*ends:\ //" | tr '\n' ' ') && \
    rm -rf /var/lib/apt/lists/*

# Copy just our installation directory from build image
COPY --from=build ${LLVM_WEDLOCK_INSTALL_DIR} ${LLVM_WEDLOCK_INSTALL_DIR}

# Let others know where we installed it
ENV LLVM_WEDLOCK_INSTALL_DIR="${LLVM_WEDLOCK_INSTALL_DIR}" \
    LLVM_BIN="${LLVM_WEDLOCK_INSTALL_DIR}/bin" \
    LLVM_DIR="${LLVM_WEDLOCK_INSTALL_DIR}/lib/cmake/llvm" \
    PATH="${LLVM_WEDLOCK_INSTALL_DIR}/bin:${PATH}"
