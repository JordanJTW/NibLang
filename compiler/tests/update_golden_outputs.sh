#!/usr/bin/env bash

# Exit on undefined variables and pipeline errors
set -uo pipefail

# Default to compiler/tests if no directory is provided
TARGET_DIR="${1:-compiler/tests}"
COMPILER="build/compiler/compiler"

# Ensure we are running from the workspace root
if [ ! -f "$COMPILER" ]; then
    echo "Error: Compiler not found at $COMPILER."
    echo "Please run this script from the workspace root."
    exit 1
fi

echo "Updating golden .out files in $TARGET_DIR..."

for file in "$TARGET_DIR"/*.nib; do
    # Skip if no .nib files exist in the directory
    [ -e "$file" ] || continue

    # Resolve the absolute path to exactly match what `lit`'s %s produces
    ABS_PATH="$(cd "$(dirname "$file")" && pwd)/$(basename "$file")"

    echo " -> Generating $(basename "$file").out"

    # Appends '|| true' because the compiler is expected to return non-zero exit
    # code 1 for these intentional errors, which would otherwise halt the script.
    "$COMPILER" "$ABS_PATH" --disable-logging 2>&1 | \
        sed "s|$ABS_PATH|[[PATH]]|g" > "${file}.out" || true

done

echo "Updated 🐙"