# Build Stage
FROM --platform=linux/amd64 ubuntu:20.04 as builder

## Install build dependencies.
RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y cmake clang libcurl4-openssl-dev zlib1g zlib1g-dev

## Add source code to the build stage.
ADD . /crashpad
WORKDIR /crashpad

RUN mkdir cbuild && \
          cmake -S . -B cbuild/ -DBUILD_EXAMPLES=TRUE && \
          cmake --build cbuild/

# Package Stage
FROM --platform=linux/amd64 ubuntu:20.04

## TODO: Change <Path in Builder Stage>
COPY --from=builder /crashpad/cbuild/handler/handler /

