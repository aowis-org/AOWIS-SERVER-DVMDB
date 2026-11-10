# Debian 13 target environment. Source is provided read-only when running.
FROM debian:trixie-slim

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update -o APT::Update::Error-Mode=any \
 && apt-get install --yes --no-install-recommends \
      build-essential \
      ca-certificates \
      cmake \
      debhelper \
      dpkg-dev \
      libssl-dev \
      ninja-build \
      pkg-config \
      python3 \
      qt6-base-dev \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /tmp
