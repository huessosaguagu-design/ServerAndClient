# ── Build C++ relay ─────────────────────────────────────────────────
FROM gcc:13 AS builder
WORKDIR /build
COPY relay/ ./relay/
COPY common/ ./common/
RUN g++ -std=c++17 -O2 -I common -o relay relay/main.cpp common/network.cpp -lpthread

# ── Runtime ────────────────────────────────────────────────────────
FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends libstdc++6 \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY --from=builder /build/relay .
EXPOSE 10000
CMD ["./relay"]
