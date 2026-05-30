# Build stage
FROM golang:1.22-alpine AS builder
WORKDIR /app
COPY go.mod ./
COPY . .
RUN CGO_ENABLED=0 go build -o /proximia ./cmd/proximia

# Runtime stage
FROM alpine:3.19
RUN apk add --no-cache ca-certificates wget
COPY --from=builder /proximia /usr/local/bin/proximia
EXPOSE 8080
VOLUME ["/data"]
ENTRYPOINT ["proximia"]
CMD ["-wal", "/data/proximia.wal"]
