# Convenience targets; everything is plain CMake + Python underneath.
.PHONY: build test test-all quick reproduce clean

build:
	cmake --preset release >/dev/null && cmake --build --preset release

test: build
	ctest --preset release

# Debug + release + UBSan (AddressSanitizer: cmake --preset asan).
test-all:
	cmake --preset dev >/dev/null && cmake --build --preset dev && ctest --preset dev
	cmake -S . -B build/ubsan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCEIO_SANITIZE=undefined >/dev/null
	cmake --build build/ubsan && UBSAN_OPTIONS=halt_on_error=1 build/ubsan/tests/ceio_tests
	$(MAKE) test

quick:
	scripts/reproduce.sh --quick

reproduce:
	scripts/reproduce.sh

clean:
	rm -rf build results
