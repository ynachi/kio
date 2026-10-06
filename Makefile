# kio: one command per task. Run `make help` for the list.
# The compiler comes from the CC and CXX environment variables on first configure.

PRESETS      := dev asan tsan release
IMAGE        ?= kio-dev
CLANG_FORMAT ?= clang-format
CLANG_TIDY   ?= run-clang-tidy
CCACHE_HOST  ?= $(HOME)/.cache/kio-ccache
TTY          := $(shell [ -t 0 ] && echo -it)

# The container sees the repository at /src. Its build trees go to ./build-docker on the
# host, so host builds (./build) and container builds never share a CMake cache.
# memlock: io_uring mlocks its rings, and Docker caps the limit at 8 MB by
# default. IoContext's default 16800 entries exceed that once a test creates
# several workers, failing with "io_uring_queue_init_params failed: Cannot
# allocate memory". -1 lifts the cap for the container.
DOCKER_RUN = docker run --rm $(TTY) \
	--security-opt seccomp=unconfined \
	--ulimit memlock=-1 \
	-u $(shell id -u):$(shell id -g) -e HOME=/tmp -e CCACHE_DIR=/ccache \
	-v $(CURDIR):/src -v $(CURDIR)/build-docker:/src/build -v $(CCACHE_HOST):/ccache \
	-w /src $(IMAGE)

.DEFAULT_GOAL := help
.PHONY: help $(PRESETS) build test check t bench fmt fmt-check tidy compdb clean docker-image docker-shell

help: ## Show this list
	@grep -hE '^[a-zA-Z%_ -]+:.*## ' $(MAKEFILE_LIST) | awk -F':.*## ' '{printf "  %-16s %s\n", $$1, $$2}'

dev asan tsan release: ## Configure, build and test that preset
	cmake --workflow --preset $@

build: ## Configure and build the dev preset without running tests
	cmake --preset dev && cmake --build --preset dev

test: dev ## Same as `make dev`

check: asan tsan ## What CI runs before a merge: both sanitizer builds

t: ## Run tests matching a pattern in the dev build: make t T=context_remote
	cmake --build --preset dev && ctest --preset dev -R '$(T)'

bench: ## Build the optimized benchmark server (never benchmark a sanitizer build)
	cmake --preset release && cmake --build --preset release --target kio_http_bench
	@echo "run: taskset -c 0-3 build/release/kio_http_bench --workers=4 --no-dispatch"

fmt: ## Format every tracked C++ file in place
	git ls-files '*.cc' '*.cpp' '*.h' '*.hpp' ':!include/libs' ':!external_libraries' | xargs $(CLANG_FORMAT) -i

fmt-check: ## Fail if any file is not formatted
	git ls-files '*.cc' '*.cpp' '*.h' '*.hpp' ':!include/libs' ':!external_libraries' | xargs $(CLANG_FORMAT) --dry-run --Werror

tidy: build ## Run clang-tidy over src/ using the dev compile database
	$(CLANG_TIDY) -p build/dev -quiet 'src/.*'

compdb: build ## Link compile_commands.json at the repo root for editors that want it there
	ln -sf build/dev/compile_commands.json compile_commands.json

clean: ## Remove every build tree
	rm -rf build build-docker compile_commands.json

docker-image: ## Build the pinned toolchain image
	docker build -t $(IMAGE) docker

docker-shell: docker-image ## Open a shell in the toolchain container
	@mkdir -p build-docker $(CCACHE_HOST)
	$(DOCKER_RUN) bash

docker-%: docker-image ## Run any target above inside the container: make docker-tsan
	@mkdir -p build-docker $(CCACHE_HOST)
	$(DOCKER_RUN) make $*
