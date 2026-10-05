# Pinned toolchain for kio. Build with `make docker-image`, use with `make docker-<target>`.
#
# io_uring: Docker's default seccomp profile blocks the io_uring system calls, so the
# container must be started with `--security-opt seccomp=unconfined` (the Makefile does).
# The host kernel must be 6.1 or newer; the container shares it.
FROM ubuntu:24.04

ARG LLVM=20
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
        ca-certificates wget gnupg lsb-release software-properties-common \
 && wget -qO /tmp/llvm.sh https://apt.llvm.org/llvm.sh \
 && bash /tmp/llvm.sh ${LLVM} \
 && apt-get install -y --no-install-recommends \
        clang-${LLVM} clang-tidy-${LLVM} clang-format-${LLVM} clangd-${LLVM} libclang-rt-${LLVM}-dev \
        libstdc++-14-dev \
        cmake ninja-build make git pkg-config \
        ccache mold \
        gdb wrk \
 && rm -rf /var/lib/apt/lists/* /tmp/llvm.sh

# Unversioned names, so the Makefile and editors need no version suffix.
RUN for t in clang clang++ clang-format clang-tidy clangd run-clang-tidy; do \
        [ -e /usr/bin/$t-${LLVM} ] && ln -sf /usr/bin/$t-${LLVM} /usr/local/bin/$t; \
    done; true

ENV CC=clang-${LLVM} CXX=clang++-${LLVM}
# The repository is mounted with the host user's uid.
RUN git config --system --add safe.directory '*'
WORKDIR /src
