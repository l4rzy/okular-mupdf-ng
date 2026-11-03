.PHONY: all dev gcc release asan test fuzz format clean

FUZZ_BUILD_DIR ?= build-ipc-fuzz
FUZZ_CXX ?= clang++
FUZZ_ARGS ?= -max_len=65536 -rss_limit_mb=2048 -max_total_time=30

all: dev

dev:
	@./scripts/build.sh dev

gcc:
	@./scripts/build.sh gcc

release:
	@./scripts/build.sh release

asan:
	@./scripts/build.sh asan

test:
	@./scripts/build.sh test

fuzz:
	cmake -S tests/fuzz -B "$(FUZZ_BUILD_DIR)" -DCMAKE_CXX_COMPILER="$(FUZZ_CXX)" -DCMAKE_BUILD_TYPE=Debug
	cmake --build "$(FUZZ_BUILD_DIR)"
	ctest --test-dir "$(FUZZ_BUILD_DIR)" --output-on-failure
	"$(FUZZ_BUILD_DIR)/ipc_fuzz_seeds" "$(FUZZ_BUILD_DIR)/corpus-codec"
	"$(FUZZ_BUILD_DIR)/test_ipc_fuzz_framing" "$(FUZZ_BUILD_DIR)/corpus-framing"
	"$(FUZZ_BUILD_DIR)/fuzz_ipc_codec" "$(FUZZ_BUILD_DIR)/corpus-codec" -artifact_prefix="$(FUZZ_BUILD_DIR)/" $(FUZZ_ARGS)
	"$(FUZZ_BUILD_DIR)/fuzz_ipc_framing" "$(FUZZ_BUILD_DIR)/corpus-framing" -artifact_prefix="$(FUZZ_BUILD_DIR)/" $(FUZZ_ARGS)

format:
	@./scripts/build.sh format

clean:
	@./scripts/build.sh clean
