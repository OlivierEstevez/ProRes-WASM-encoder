#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

echo "Building ProRes WASM Encoder..."

cd "$PROJECT_DIR"

# Build Docker image
docker build -t prores-wasm-builder -f build/Dockerfile .

# Run build. Create dist/ first: if Docker creates the mount point it is
# owned by root on Linux, and the JS build can't write into it afterwards.
mkdir -p "$PROJECT_DIR/dist"
docker run --rm -v "$PROJECT_DIR/dist:/src/dist" prores-wasm-builder

echo "Build complete! Output in dist/"
