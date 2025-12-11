#!/bin/bash

# Configuration
WASI_SDK_PATH="/opt/wasi-sdk"
CC="${WASI_SDK_PATH}/bin/clang"
OUTPUT_DIR="../wasm_assets"

# Ensure output directory exists
mkdir -p "$OUTPUT_DIR"

# Compilation flags
# -O3: Optimization
# --target=wasm32-wasi: Target WebAssembly with WASI
# -Wl,--allow-undefined: Allow undefined symbols (for host functions)
# -Wl,--export=main: Export main function
# Note: initial-memory must be multiple of 65536 (WASM page size)
# Using minimum 1 page (65536 bytes) for ESP32 memory constraints
CFLAGS="-O3 \
    --target=wasm32-wasi \
    -Wl,--initial-memory=65536 \
    -Wl,--max-memory=65536 \
    -z stack-size=2048 \
    -Wl,--export=main \
    -Wl,--allow-undefined "

compile_file() {
    local input_file="$1"
    local filename=$(basename -- "$input_file")
    local name="${filename%.*}"
    local output_file="$OUTPUT_DIR/$name.wasm"

    echo "Compiling $input_file to $output_file..."
    
    if [ ! -x "$CC" ]; then
        echo "Error: Compiler not found at $CC"
        echo "Please ensure WASI SDK is installed at $WASI_SDK_PATH"
        exit 1
    fi

    "$CC" $CFLAGS -o "$output_file" "$input_file"
    
    if [ $? -eq 0 ]; then
        echo "Success: $output_file"
    else
        echo "Failed to compile $input_file"
        exit 1
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
echo "Compiled binaries:"
ls -lh "$OUTPUT_DIR"/*.wasm 2>/dev/null
