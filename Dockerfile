# docker build -t alpine-chess:3.22 --rm .
ARG ALPINE_VER="3.22"
ARG ALPINE_ARCH="arm64v8"
FROM $ALPINE_ARCH/alpine:$ALPINE_VER
LABEL version="3.22"
ARG TZ="Australia/Brisbane"
RUN apk update && \
    apk upgrade && \
    apk add tzdata alpine-conf coreutils sudo sqlite-dev g++ gdb valgrind && \
    echo "alpine ALL=(ALL) NOPASSWD: ALL" >> /etc/sudoers && \
    setup-timezone $TZ && \
    addgroup alpine && \
    adduser -G alpine -D alpine && \
    sed -i -e '/^alpine/s/!/*/' /etc/shadow
USER alpine
VOLUME /libchess
WORKDIR /home/alpine
