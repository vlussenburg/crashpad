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

RUN cp /crashpad/cbuild/handler/handler /

# Package Stage
FROM --platform=linux/amd64 ubuntu:20.04

COPY --from=builder /crashpad/cbuild/examples/linux/demo/demo_linux /
COPY --from=builder /crashpad/cbuild/handler/handler /
RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y libcurl4-openssl-dev zlib1g
