# ---- Build stage ----
FROM alpine:3.21 AS builder

RUN apk add --no-cache \
    cmake make g++ \
    highway-dev nlohmann-json cpp-httplib-dev \
    spdlog-dev cli11-dev yaml-cpp-dev \
    openssl-dev zlib-dev

WORKDIR /app
COPY src/ ./src/
COPY CMakeLists.txt vcpkg.json ./
COPY web/ ./web/

RUN cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF && \
    cmake --build build -j$(nproc) && \
    strip build/src/flux

# ---- Runtime stage ----
FROM alpine:3.21

RUN apk add --no-cache ca-certificates
COPY --from=builder /app/build/src/flux /usr/local/bin/flux
COPY web/ /app/web/

EXPOSE 9876
VOLUME ["/data"]
ENV FLUX_WAL_PATH=/data/flux.wal

ENTRYPOINT ["flux"]
CMD ["--addr", ":9876", "--wal", "/data/flux.wal"]
