# syntax=docker/dockerfile:1
FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    ca-certificates \
    cmake \
    git \
    libavcodec-dev \
    libavformat-dev \
    libavutil-dev \
    libcurl4-openssl-dev \
    libevent-dev \
    libjson-c-dev \
    libminiupnpc-dev \
    libopus-dev \
    libssl-dev \
    pkg-config \
    protobuf-compiler \
    python3 \
    python3-protobuf \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr/local \
    -DCHIAKI_ENABLE_GUI=OFF \
    -DCHIAKI_ENABLE_CLI=ON \
    -DCHIAKI_ENABLE_TESTS=OFF \
    -DCHIAKI_ENABLE_BOREALIS=OFF \
    -DCHIAKI_ENABLE_ANDROID=OFF \
    -DCHIAKI_ENABLE_FFMPEG_DECODER=OFF \
    -DCHIAKI_ENABLE_PI_DECODER=OFF \
    -DCHIAKI_ENABLE_STEAMDECK_NATIVE=OFF \
    -DCHIAKI_ENABLE_SETSU=OFF \
    -DCHIAKI_ENABLE_STEAM_SHORTCUT=OFF \
    -DCHIAKI_ENABLE_SPEEX=OFF \
    -DCHIAKI_LIB_ENABLE_OPUS=ON \
    -DCHIAKI_USE_SYSTEM_CURL=ON \
    && cmake --build build --target chiaki-cli -j"$(nproc)" \
    && install -D -m 0755 build/cli/chiaki-cli /out/usr/local/bin/chiaki-cli

FROM golang:alpine AS mediamtx-builder
RUN apk add --no-cache git \
    && git clone --depth 1 --branch v1.21.4 https://github.com/bluenviron/mediamtx.git /mediamtx \
    && cd /mediamtx \
    && go generate ./... \
    && go mod download \
    && chmod -R u+w /go/pkg/mod/github.com/bluenviron/mediacommon* \
    && sed -i 's/MaxNALUsPerAccessUnit = 50/MaxNALUsPerAccessUnit = 256/g' /go/pkg/mod/github.com/bluenviron/mediacommon/v2@*/pkg/codecs/h264/h264.go \
    && sed -i 's/MaxNALUsPerAccessUnit = 21/MaxNALUsPerAccessUnit = 256/g' /go/pkg/mod/github.com/bluenviron/mediacommon/v2@*/pkg/codecs/h265/h265.go \
    && CGO_ENABLED=0 go build -ldflags="-s -w" -o /mediamtx-bin .

FROM ubuntu:24.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    libavcodec60 \
    libavformat60 \
    libavutil58 \
    libcurl4t64 \
    libevent-2.1-7t64 \
    libjson-c5 \
    libminiupnpc17 \
    libopus0 \
    libssl3t64 \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd -g 10001 chiaki \
    && useradd -u 10001 -g 10001 -M -s /usr/sbin/nologin chiaki

COPY --from=builder /out/usr/local/ /usr/local/
COPY --from=mediamtx-builder /mediamtx-bin /usr/local/bin/mediamtx
RUN ldconfig

USER 10001:10001
ENTRYPOINT ["chiaki-cli"]
CMD ["--help"]
