#!/bin/bash

if [ $# -lt 1 ]; then
    echo "Usage: $0 <input.avi> [output_directory]"
    exit 1
fi

INPUT="$1"
OUTPUT_DIR="${2:-output}"

if [ ! -f "$INPUT" ]; then
    echo "Error: File not found: $INPUT"
    exit 1
fi

mkdir -p "$OUTPUT_DIR"

ffmpeg -i "$INPUT" "$OUTPUT_DIR/frame_%06d.jpg"
