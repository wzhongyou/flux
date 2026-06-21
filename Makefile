.PHONY: build run test clean docker stop

# Build the binary
build:
	go build -o bin/flux ./cmd/flux

# Run the server locally
run:
	go run ./cmd/flux

# Run all tests
test:
	go test ./...

# Clean build artifacts
clean:
	rm -rf bin/ data/ *.wal *.snapshot.json

# Build Docker image
docker:
	docker build -t flux:latest .

# Start with Docker Compose
docker-up:
	docker compose up -d

# Stop Docker Compose
docker-down:
	docker compose down

# Build and run
all: build run
