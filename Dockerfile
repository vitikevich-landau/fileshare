# --- Build stage: compile the v2 daemon (and the v1 server) in Release -------
# Build with SHA-256 checksums via: docker build --build-arg USE_SHA256=ON .
FROM ubuntu:24.04 AS build
ARG USE_SHA256=OFF
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        cmake ninja-build g++ git nlohmann-json3-dev libssl-dev ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
# Headless build: no FTXUI/TUI on the server (FILESHARE_BUILD_TUI=OFF).
RUN cmake -S . -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DFILESHARE_BUILD_TESTS=OFF \
        -DFILESHARE_BUILD_TUI=OFF \
        -DFILESHARE_USE_SHA256=${USE_SHA256} \
    && cmake --build build --target fileshare_daemon_app fileshare_server_app

# --- Runtime stage: the daemon binaries -------------------------------------
FROM ubuntu:24.04
ARG USE_SHA256=OFF
ENV DEBIAN_FRONTEND=noninteractive
# libssl3 is only needed for the SHA-256 build.
RUN if [ "$USE_SHA256" = "ON" ]; then \
        apt-get update && apt-get install -y --no-install-recommends libssl3 \
        && rm -rf /var/lib/apt/lists/*; \
    fi
RUN useradd --create-home --uid 10001 fileshare
COPY --from=build /src/build/fileshare-daemon  /usr/local/bin/fileshare-daemon
COPY --from=build /src/build/fileshare_server   /usr/local/bin/fileshare_server
USER fileshare
WORKDIR /data
EXPOSE 5555
# Served tree under /data/share, config/users/cache under /data (mount a volume).
# SIGTERM triggers a graceful drain -- give docker enough stop grace (see compose).
STOPSIGNAL SIGTERM
ENTRYPOINT ["fileshare-daemon"]
CMD ["--config", "/data/config.json", "--share-root", "/data/share", "--port", "5555"]
