# A Linux build and test environment: the same Ubuntu, compilers, and CMake
# as CI, plus LevelDB and RocksDB so db_bench can compare against them.
#
#   docker build -t lsm .
#   docker run --rm lsm                        # debug build, all tests, with g++
#   docker run --rm -e PRESET=asan lsm         # or release, tsan
#   docker run --rm -e CXX=clang++ lsm         # with clang
#
# For working on the code, compose.yaml mounts the checkout instead of
# copying it.

FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
        clang \
        cmake \
        g++ \
        git \
        libclang-rt-dev \
        libleveldb-dev \
        librocksdb-dev \
        ninja-build \
        python3 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

ENV PRESET=debug \
    CXX=g++
CMD ["bash", "-c", "cmake --preset \"$PRESET\" && cmake --build --preset \"$PRESET\" && ctest --preset \"$PRESET\" -j \"$(nproc)\""]
