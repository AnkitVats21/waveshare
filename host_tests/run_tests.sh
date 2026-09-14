#!/usr/bin/env bash
set -e

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$DIR"

# Ensure venv exists
if [ ! -d ".venv" ]; then
    echo "Creating virtual environment in host_tests/.venv..."
    python3 -m venv .venv
    .venv/bin/pip install pytest nanobind
fi

# Build native bindings module
echo "Building waveshare_host native module..."
cmake -B build -S . -DPython_EXECUTABLE="$DIR/.venv/bin/python3"
cmake --build build -j"$(nproc)"

# Run pytest
echo "Running test suite..."
.venv/bin/pytest tests/ -v "$@"
