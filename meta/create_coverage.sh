#!/usr/bin/env bash

set -e  # Exit on command failure

BUILD_DIR="build/cov-build"
OUTPUT_DIR="coverage_html"
IGNORE_REGEX="(googletest|_test\.cc|logging\.(cc|h))"

rm -rf "$OUTPUT_REPORT_DIR"
# Remove old raw profiles if the build folder already exists
if [ -d "$BUILD_DIR" ]; then
  find "$BUILD_DIR" -name "code-*.profraw" -delete
  rm -f "$BUILD_DIR/merged.profdata"
fi

cmake -B "$BUILD_DIR" -S . -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DENABLE_COVERAGE=ON

ninja -C $BUILD_DIR

# Get the absolute path to the output directory before we change directories
ABS_OUTPUT_DIR=$(mkdir -p "$OUTPUT_DIR" && cd "$OUTPUT_DIR" && pwd)

cd "$BUILD_DIR"

export LLVM_PROFILE_FILE="code-%p.profraw"
ctest > /dev/null 2>&1 || echo "⚠️  Warning: Some tests failed, generating report anyway..."

xcrun llvm-profdata merge -sparse **/code-*.profraw -o merged.profdata

xcrun llvm-cov show \
    $(find ./compiler ./test -type f -name "*_test" -exec echo -object {} \;) \
    -instr-profile=merged.profdata \
    -ignore-filename-regex=$IGNORE_REGEX \
    -output-dir $ABS_OUTPUT_DIR \
    -format=html

echo "🐙 Done!"