#!/bin/bash

set -euo pipefail

output_file="${1:-test_1gb.bin}"
block_size=$((1024 * 1024))
block_count=1024
total_size=$((block_size * block_count))

echo "Creating ${output_file} with size 1 GiB (${total_size} bytes)..."

truncate -s "${total_size}" "${output_file}"

for block_index in $(seq 0 $((block_count - 1))); do
    marker=$(printf 'BLOCK:%04d OFFSET:%012d\n' "${block_index}" "$((block_index * block_size))")
    printf '%s' "${marker}" | dd of="${output_file}" bs="${block_size}" seek="${block_index}" conv=notrunc status=none
done

sha256sum "${output_file}" > "${output_file}.sha256"

echo "Created ${output_file} (${total_size} bytes)"
echo "Checksum saved to ${output_file}.sha256"
echo "Quick checks:"
echo "  ls -lh ${output_file}"
echo "  strings -n 24 ${output_file} | head"
echo "  sha256sum -c ${output_file}.sha256"