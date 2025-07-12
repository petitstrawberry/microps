FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

ENV USER=root

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        build-essential \
        git \
        iproute2 \
        iputils-ping \
        netcat-openbsd

WORKDIR /workspace