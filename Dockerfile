# Build stage
FROM golang:1.22-alpine AS builder
WORKDIR /app
COPY go.mod ./
COPY . .
RUN CGO_ENABLED=0 go build -o /flux ./cmd/flux

# Runtime stage
FROM alpine:3.19
RUN apk add --no-cache ca-certificates wget
COPY --from=builder /flux /usr/local/bin/flux
EXPOSE 8080
VOLUME ["/data"]
ENTRYPOINT ["flux"]
CMD ["-wal", "/data/flux.wal"]
