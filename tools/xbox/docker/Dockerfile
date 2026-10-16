# N64Recomp-X SDK image (PLAN.md 4.1): the toolchain only, never a game or a
# ROM. nxdk built from source at the pinned commit with LLVM 21 from
# apt.llvm.org (Debian's clang 19.1.7 is in the range nxdk warns about,
# llvm/llvm-project#134607), N64Recomp at the submodule's pin, Python with
# rabbitizer and Pillow for n64rx. Builds natively for linux/amd64 and
# linux/arm64 (Apple Silicon runs the arm64 image at full speed):
#   docker buildx build --platform linux/amd64,linux/arm64 -t ghcr.io/techprogabe/n64recomp-x-sdk:<tag> \
#     -f tools/xbox/docker/Dockerfile .
# n64rx runs its whole pipeline inside with the checkout at /src, the ROM's
# folder at /rom (read-only) and the output folder at /out.
FROM debian:trixie
ARG LLVM=21
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      build-essential cmake git bison flex python3 python3-pip patch ca-certificates wget gnupg \
      pkg-config file xxd ninja-build libzstd-dev zlib1g-dev libxml2-dev \
 && wget -qO- https://apt.llvm.org/llvm-snapshot.gpg.key > /etc/apt/trusted.gpg.d/apt.llvm.org.asc \
 && echo "deb http://apt.llvm.org/trixie/ llvm-toolchain-trixie-${LLVM} main" > /etc/apt/sources.list.d/llvm.list \
 && apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      clang-${LLVM} lld-${LLVM} llvm-${LLVM} \
 && rm -rf /var/lib/apt/lists/*
ENV LLVM=/usr/lib/llvm-${LLVM}
ENV PATH=${LLVM}/bin:$PATH
ARG NXDK_SHA=58427c078b4ccb0121f359b6b6c536b3b2914976
RUN git clone --recursive https://github.com/XboxDev/nxdk.git /usr/src/nxdk \
 && cd /usr/src/nxdk && git checkout -q $NXDK_SHA && git submodule update --init --recursive
ENV NXDK_DIR=/usr/src/nxdk
RUN cd /usr/src/nxdk && make tools -j$(nproc)
RUN bash -c 'eval $(/usr/src/nxdk/bin/activate -s); cd /usr/src/nxdk && make NXDK_ONLY=y NXDK_SDL=y NXDK_CXX=y -j$(nproc)'
# nxdk's Cg compiler (tools/cg/linux/cgc) is an i386 binary; on arm64 it runs
# under qemu-i386 binfmt and needs the i386 runtime
RUN if [ "$(dpkg --print-architecture)" = amd64 ]; then dpkg --add-architecture i386 && apt-get update \
      && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends libc6:i386 libstdc++6:i386 \
      && rm -rf /var/lib/apt/lists/*; fi
RUN pip3 install --break-system-packages --no-cache-dir rabbitizer pillow
# N64Recomp at the checkout's submodule pin: copied in at build time (the
# build context is the repo root), built to /opt/n64recomp; n64rx inside the
# image finds it there through N64RX_RECOMP
COPY tools/recomp /usr/src/n64recomp
RUN cmake -S /usr/src/n64recomp -B /opt/n64recomp -G Ninja -DCMAKE_BUILD_TYPE=Release \
 && ninja -C /opt/n64recomp N64RecompCLI RSPRecomp && rm -rf /opt/n64recomp/CMakeFiles
ENV N64RX_RECOMP=/opt/n64recomp N64RX_IN_DOCKER=1
WORKDIR /src
