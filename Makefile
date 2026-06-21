.PHONY: build run test clean docker debug deps format

BUILD_DIR := build
BUILD_TYPE ?= Release
NPROC := $(shell nproc 2>/dev/null || sysctl -n hw.logicalcpu 2>/dev/null || echo 4)

build:
	cmake -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(BUILD_TYPE)
	cmake --build $(BUILD_DIR) -j$(NPROC)

run: build
	./$(BUILD_DIR)/src/flux

test: build
	cd $(BUILD_DIR) && ctest --output-on-failure -j$(NPROC)

clean:
	rm -rf $(BUILD_DIR)

docker:
	docker build -t flux:latest .

debug:
	cmake -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Debug -DSANITIZE=ON
	cmake --build $(BUILD_DIR) -j$(NPROC)

deps:
	brew install cmake nlohmann-json spdlog highway cpp-httplib cli11 yaml-cpp googletest

format:
	find src tests -name '*.cpp' -o -name '*.h' | xargs clang-format -i
