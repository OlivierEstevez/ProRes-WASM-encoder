cmake_minimum_required(VERSION 3.14)
project(prores-wasm C)

set(CMAKE_C_STANDARD 11)

# Source files
set(ENCODER_SOURCES
    src/encoder/prores_encoder.c
    src/encoder/prores_dct.c
    src/encoder/prores_vlc.c
)

set(MUXER_SOURCES
    src/muxer/mov_muxer.c
)

set(WASM_SOURCES
    src/wasm/bindings.c
)

# All sources
set(ALL_SOURCES
    ${ENCODER_SOURCES}
    ${MUXER_SOURCES}
    ${WASM_SOURCES}
)

# Include directories
include_directories(
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/encoder
    ${CMAKE_SOURCE_DIR}/src/muxer
)

if(EMSCRIPTEN)
    # Emscripten-specific settings
    set(CMAKE_EXECUTABLE_SUFFIX ".js")

    # WASM SIMD128 (universal since Chrome 91 / Firefox 89 / Safari 16.4)
    # + link-time optimization. -msimd128 lets the vectorized DCT lower to
    # native SIMD instructions and enables autovectorization elsewhere.
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -msimd128 -flto")

    # Exported functions
    set(EXPORTED_FUNCTIONS
        "_malloc"
        "_free"
        "_prores_wasm_create"
        "_prores_wasm_add_frame_rgba"
        "_prores_wasm_add_frame_yuv"
        "_prores_wasm_encode_frame_rgba"
        "_prores_wasm_encode_frame_yuv"
        "_prores_wasm_last_frame_size"
        "_prores_wasm_mux_record_sample"
        "_prores_wasm_finalize_header"
        "_prores_wasm_finalize_moov"
        "_prores_wasm_finalize"
        "_prores_wasm_destroy"
        "_prores_wasm_free_buffer"
        "_prores_wasm_alloc"
        "_prores_wasm_get_rgba_buffer_size"
        "_prores_wasm_get_yuv_buffer_size"
        "_prores_wasm_get_fourcc"
    )

    # Join exported functions
    string(REPLACE ";" "," EXPORTED_FUNCTIONS_STR "${EXPORTED_FUNCTIONS}")

    # Emscripten link flags
    set(EMSCRIPTEN_LINK_FLAGS
        "-s WASM=1"
        "-s MODULARIZE=1"
        "-s EXPORT_NAME='createProResModule'"
        "-s EXPORT_ES6=1"
        # The .wasm binary is kept separate (not base64-embedded in the glue)
        # so the JS build can embed it exactly once and share the compiled
        # module between the main thread and workers (instantiateWasm hook).
        "-s SINGLE_FILE=0"
        "-s ALLOW_MEMORY_GROWTH=1"
        "-s INITIAL_MEMORY=67108864"
        "-s MAXIMUM_MEMORY=4294967296"
        "-s EXPORTED_FUNCTIONS='[${EXPORTED_FUNCTIONS_STR}]'"
        "-s EXPORTED_RUNTIME_METHODS='[\"ccall\",\"cwrap\",\"getValue\",\"setValue\",\"UTF8ToString\",\"HEAPU8\",\"HEAPU32\"]'"
        "-s NO_EXIT_RUNTIME=1"
        "-s FILESYSTEM=0"
        "-O3"
        "-msimd128"
        "-flto"
        "--closure 1"
    )

    string(REPLACE ";" " " EMSCRIPTEN_LINK_FLAGS_STR "${EMSCRIPTEN_LINK_FLAGS}")

    add_executable(prores-encoder.wasm ${ALL_SOURCES})
    set_target_properties(prores-encoder.wasm PROPERTIES LINK_FLAGS "${EMSCRIPTEN_LINK_FLAGS_STR}")

else()
    # Native build for testing
    add_library(prores-encoder STATIC ${ENCODER_SOURCES} ${MUXER_SOURCES})

    # Test executable
    add_executable(prores-test test/test_native.c)
    target_link_libraries(prores-test prores-encoder)
endif()
