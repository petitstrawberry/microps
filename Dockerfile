FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

ENV USER=root

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        ca-certificates \
        build-essential \
        git \
        iproute2 \
        iputils-ping \
        netcat-openbsd

RUN apt-get clean && \
    rm -rf /var/lib/apt/lists/*

RUN update-ca-certificates

WORKDIR /workspace