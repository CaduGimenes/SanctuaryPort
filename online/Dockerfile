# Builds the standalone PSN master and the Demonware title server into one
# runtime image. The compose file starts each binary as its own service.
#
# Build context: online/ (docker compose does this from this directory).

FROM golang:1.24-alpine AS build
WORKDIR /src
ENV CGO_ENABLED=0

# Download module dependencies in their own layers. The two projects are
# independent Go modules, so each has its own go.mod/go.sum.
COPY psn/go.mod psn/go.sum ./psn/
COPY demonware/go.mod demonware/go.sum ./demonware/
RUN cd psn && go mod download && \
    cd ../demonware && go mod download

COPY psn/ ./psn/
COPY demonware/ ./demonware/
RUN cd psn && go build -buildvcs=false -trimpath -ldflags='-s -w' \
        -o /out/d3master ./cmd/d3master && \
    cd ../demonware && go build -buildvcs=false -trimpath -ldflags='-s -w' \
        -o /out/d3demonware ./cmd/d3demonware

FROM alpine:3.22 AS runtime
RUN apk add --no-cache ca-certificates \
    && addgroup -S -g 10001 d3 \
    && adduser -S -D -H -u 10001 -G d3 d3 \
    && mkdir -p /app /data \
    && chown -R d3:d3 /app /data
WORKDIR /app
COPY --from=build --chown=d3:d3 /out/d3master /out/d3demonware /app/
COPY --chown=d3:d3 LICENSE NOTICE /app/
USER d3
VOLUME ["/data"]

# PSN master and Matching2.
EXPOSE 31313/tcp 31314/udp 31315/tcp 3478/udp
# Demonware lobby, title auth and NAT discovery.
EXPOSE 3074/tcp 31316/tcp 31317/udp 31318/udp

# The default command runs the PSN master; compose selects d3demonware for the
# second service. Both write their JSON config and state under /data.
CMD ["/app/d3master", "-config", "/data/d3master.json"]
