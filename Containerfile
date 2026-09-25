# Build stage: compile the gym journal Telegram bot.
FROM debian:bookworm AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        pkg-config \
        libcurl4-openssl-dev \
        libpq-dev \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .
RUN make

# Runtime stage: only the shared libraries needed to run the bot.
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
        libcurl4 \
        libpq5 \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=build /src/gymjournal /app/gymjournal

CMD ["/app/gymjournal"]
