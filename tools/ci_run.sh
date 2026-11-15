#!/usr/bin/env bash
# Local CI: sanitizer builds, the full GoogleTest suite, a TSan concurrency run,
# and a short experiment pass.  Mirrors .github/workflows/ci.yml.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

echo "==> Debug + ASan/UBSan build"
cmake -S . -B build-ci -DCMAKE_BUILD_TYPE=Debug -DNVMESS_SANITIZE=ON \
      -DNVMESS_BUILD_TESTS=ON
cmake --build build-ci -j
ctest --test-dir build-ci --output-on-failure

echo "==> ThreadSanitizer build + concurrency test"
cmake -S . -B build-ci-tsan -DCMAKE_BUILD_TYPE=Debug -DNVMESS_TSAN=ON \
      -DNVMESS_BUILD_TESTS=ON
cmake --build build-ci-tsan -j --target test_concurrency
./build-ci-tsan/tests/test_concurrency

echo "==> Release build and short experiment pass"
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DNVMESS_BUILD_TESTS=ON
cmake --build build -j
python3 tools/run_experiments.py --quick

echo "ci_run.sh: all stages passed"
