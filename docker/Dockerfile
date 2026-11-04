# Native arm64 Linux build image for `make original`.
# The pinned aarch64 toolchain is baked into /opt/agt so downloads are never part of a build timing.
FROM ubuntu:22.04

ARG TOOLCHAIN_URL=https://developer.arm.com/-/media/Files/downloads/gnu-rm/10.3-2021.10/gcc-arm-none-eabi-10.3-2021.10-aarch64-linux.tar.bz2

RUN apt-get update \
 && apt-get install -y --no-install-recommends make python3 bzip2 wget ca-certificates git \
 && rm -rf /var/lib/apt/lists/*

RUN mkdir -p /opt/agt \
 && wget -q -O /tmp/toolchain.tar.bz2 "$TOOLCHAIN_URL" \
 && tar -xjf /tmp/toolchain.tar.bz2 -C /opt/agt --strip-components=1 \
 && rm /tmp/toolchain.tar.bz2 \
 && /opt/agt/bin/arm-none-eabi-gcc --version | head -1

# Bind-mounted repos are owned by a different uid, let git (used for BUILD_TAG) read them anyway.
RUN git config --system --add safe.directory '*'

ENV TOOLCHAIN=/opt/agt/bin/arm-none-eabi-
WORKDIR /work
