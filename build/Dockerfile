# ProRes WASM Encoder Build Environment
FROM emscripten/emsdk:3.1.50

WORKDIR /src

# Install additional build tools
RUN apt-get update && apt-get install -y \
    cmake \
    ninja-build \
    && rm -rf /var/lib/apt/lists/*

# Copy source files
COPY . .

# Build script
RUN mkdir -p /src/dist

CMD ["bash", "-c", "\
    mkdir -p build-wasm && \
    cd build-wasm && \
    emcmake cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release && \
    ninja && \
    cp prores-encoder.js ../dist/ \
"]
