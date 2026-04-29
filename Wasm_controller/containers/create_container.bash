#!/bin/bash

# Configuration
WASI_SDK_PATH="/opt/wasi-sdk"
CC="${WASI_SDK_PATH}/bin/clang"
WAMRC="../compilers/wamrc"
WASM_DIR="../wasm_assets/wasm"
AOT_DIR="../wasm_assets/aot"

# Ensure output directories exist
mkdir -p "$WASM_DIR"
mkdir -p "$AOT_DIR"

# Compilation flags
# -O3: Optimization
# --target=wasm32-wasi: Target WebAssembly with WASI
# -Wl,--allow-undefined: Allow undefined symbols (for host functions)
# -Wl,--export=main: Export main function
# Note: initial-memory must be multiple of 65536 (WASM page size)
# Using minimum 1 page (65536 bytes) for ESP32 memory constraints
CFLAGS="-O3 \
    --target=wasm32-wasi \
    -nostdlib \
    -I../main \
    -Wl,--initial-memory=65536 \
    -Wl,--export=__heap_base,--export=__data_end \
    -z stack-size=2048 \
    -Wl,--no-entry \
    -Wl,--export=main \
    -Wl,--export=produce,--export=consume \
    -Wl,--allow-undefined "

compile_file() {
    local input_file="$1"
    local filename=$(basename -- "$input_file")
    local name="${filename%.*}"
    local wasm_file="$WASM_DIR/$name.wasm"
    local aot_file="$AOT_DIR/$name.aot"

    # Step 1: C -> WASM
    echo "[WASM] $input_file -> $wasm_file"

    if [ ! -x "$CC" ]; then
        echo "Error: Compiler not found at $CC"
        echo "Please ensure WASI SDK is installed at $WASI_SDK_PATH"
        exit 1
    fi

    "$CC" $CFLAGS -o "$wasm_file" "$input_file"

    if [ $? -ne 0 ]; then
        echo "Failed to compile $input_file to WASM"
        exit 1
    fi

    # Step 2: WASM -> AOT
    if [ -x "$WAMRC" ]; then
        echo "[AOT]  $wasm_file -> $aot_file"
        "$WAMRC" --target=xtensa --opt-level=3 --size-level=3 -o "$aot_file" "$wasm_file"
        if [ $? -ne 0 ]; then
            echo "Failed to compile $wasm_file to AOT"
            exit 1
        fi
    else
        echo "[AOT]  Skipped (wamrc not found at $WAMRC)"
    fi
}

if [ -n "$1" ]; then
    # Compile specific file
    if [ -f "$1" ]; then
        compile_file "$1"
    else
        echo "Error: File $1 not found."
        exit 1
    fi
else
    # Compile all .c files in current directory
    echo "No file specified. Compiling all .c files in current directory..."
    found_files=false
    for file in *.c; do
        if [ -f "$file" ]; then
            compile_file "$file"
            found_files=true
        fi
    done

    if [ "$found_files" = false ]; then
        echo "No .c files found in the current directory."
    fi
fi

echo ""
echo "WASM binaries:"
ls -lh "$WASM_DIR"/*.wasm 2>/dev/null
echo ""
echo "AOT binaries:"
ls -lh "$AOT_DIR"/*.aot 2>/dev/null
