# syntax=docker/dockerfile:1

FROM ubuntu:24.04 AS build

ARG JOBS=2
ARG SEASTAR_VERSION=seastar-25.05.0

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install --yes --no-install-recommends ca-certificates git \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN DEPS_DIR=/opt/seaproxy-deps \
    BUILD_DIR=/tmp/seaproxy-build \
    JOBS="${JOBS}" \
    SEASTAR_VERSION="${SEASTAR_VERSION}" \
    ./build.sh \
    && strip /tmp/seaproxy-build/seaproxy

FROM ubuntu:24.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install --yes --no-install-recommends \
        ca-certificates \
        libboost-program-options1.83.0 \
        libboost-thread1.83.0 \
        libc-ares2 \
        libfmt9 \
        libgnutls30t64 \
        libhwloc15 \
        liburing2 \
        libyaml-cpp0.8 \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --create-home --uid 10001 seaproxy

COPY --from=build /tmp/seaproxy-build/seaproxy /usr/local/bin/seaproxy

USER 10001:10001
EXPOSE 7000
ENTRYPOINT ["/usr/local/bin/seaproxy"]
CMD ["--help"]
