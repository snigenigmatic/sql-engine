set shell := ["bash", "-cu"]

default:
  @just --list

# Configure default Debug build
configure:
  cmake -B build

# Configure explicit Debug build
configure-debug:
  cmake -B build -DCMAKE_BUILD_TYPE=Debug

# Configure Release build
configure-release:
  cmake -B build -DCMAKE_BUILD_TYPE=Release

# Configure build with tests disabled
configure-no-tests:
  cmake -B build -DBUILD_TESTS=OFF

# Configure build with logging disabled
configure-no-logging:
  cmake -B build -DENABLE_LOGGING=OFF

# Build project
build:
  cmake --build build

# Build and run all tests
test:
  ctest --test-dir build --output-on-failure

# Build + test in one command
check:
  cmake --build build && ctest --test-dir build --output-on-failure

# Verbose ctest output
test-verbose:
  ctest --test-dir build --output-on-failure --verbose

# Run focused test binaries
test-query:
  ./build/test/query_test

test-parser:
  ./build/test/parser_test

# Run SQL engine REPL
run:
  ./build/src/sqlengine

# Remove build artifacts
clean:
  rm -rf build

# No-cache build (fresh configure + build)
fresh:
  rm -rf build && cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build
