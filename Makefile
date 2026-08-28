.PHONY: all dev release asan test format clean

all: dev

dev:
	@./scripts/build.sh dev

release:
	@./scripts/build.sh release

asan:
	@./scripts/build.sh asan

test:
	@./scripts/build.sh test

format:
	@./scripts/build.sh format

clean:
	@./scripts/build.sh clean
