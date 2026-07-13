# --- Build stage: compile the epoll server (Release) ------------------------
# Build with SHA-256 checksums via: docker build --build-arg USE_SHA256=ON .
FROM ubuntu:24.04 AS build
ARG USE_SHA256=OFF
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        cmake ninja-build g++ git nlohmann-json3-dev libssl-dev ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -S . -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DFILESHARE_BUILD_TESTS=OFF \
        -DFILESHARE_USE_SHA256=${USE_SHA256} \
    && cmake --build build --target fileshare_server_app

# --- Runtime stage: just the server binary ----------------------------------
FROM ubuntu:24.04
ARG USE_SHA256=OFF
ENV DEBIAN_FRONTEND=noninteractive
# libssl3 is only needed for the SHA-256 build.
RUN if [ "$USE_SHA256" = "ON" ]; then \
        apt-get update && apt-get install -y --no-install-recommends libssl3 \
        && rm -rf /var/lib/apt/lists/*; \
    fi
RUN useradd --create-home --uid 10001 fileshare
COPY --from=build /src/build/fileshare_server /usr/local/bin/fileshare_server
USER fileshare
WORKDIR /data
EXPOSE 5555
# Shared files and config.json live under /data (mount a volume there). Append
# --add <path>[=alias] by overriding the command.
ENTRYPOINT ["fileshare_server"]
CMD ["--port", "5555", "--config", "/data/config.json"]
