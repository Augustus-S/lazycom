# syntax=docker/dockerfile:1
ARG BASE_IMAGE
FROM ${BASE_IMAGE}

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install --no-install-recommends -y \
    ca-certificates curl build-essential xz-utils bzip2 flex bison \
    libgmp-dev libmpfr-dev libmpc-dev ninja-build dpkg-dev rpm file binutils \
    desktop-file-utils squashfs-tools && rm -rf /var/lib/apt/lists/*

WORKDIR /tmp/toolchain
RUN curl -fL --retry 3 https://gcc.gnu.org/pub/gcc/releases/gcc-13.4.0/gcc-13.4.0.tar.xz -o gcc.tar.xz \
    && echo '9b4b83ecf51ef355b868608b8d257b2fa435c06d2719cb86657a7c2c2a0828ff4ce04e9bac1055bbcad8ed5b4da524cafaef654785e23a50233d95d89201e35f  gcc.tar.xz' | sha512sum -c - \
    && tar -xf gcc.tar.xz \
    && mkdir gcc-build && cd gcc-build \
    && ../gcc-13.4.0/configure --prefix=/opt/gcc --enable-languages=c,c++ \
       --disable-multilib --disable-bootstrap --disable-nls \
    && make -j2 && make install \
    && mkdir -p /opt/gcc/licenses \
    && cp ../gcc-13.4.0/COPYING3 ../gcc-13.4.0/COPYING.RUNTIME /opt/gcc/licenses/ \
    && cd / && rm -rf /tmp/toolchain

RUN arch=$(uname -m); case "$arch" in \
      x86_64) digest=3cb3dd247b6a1de2d0f4b20c6fd4326c9024e894cebc9dc8699758887e566ca7 ;; \
      aarch64) digest=a343c6294f770742904e6a6792e0956b5ff8212abfb63cac99237de2e210fa0f ;; \
      *) exit 1 ;; esac \
    && curl -fL --retry 3 "https://github.com/Kitware/CMake/releases/download/v3.31.10/cmake-3.31.10-linux-$arch.tar.gz" -o /tmp/cmake.tar.gz \
    && echo "$digest  /tmp/cmake.tar.gz" | sha256sum -c - \
    && mkdir /opt/cmake && tar -xf /tmp/cmake.tar.gz -C /opt/cmake --strip-components=1 \
    && rm /tmp/cmake.tar.gz
ENV PATH="/opt/gcc/bin:/opt/cmake/bin:${PATH}"
WORKDIR /src
