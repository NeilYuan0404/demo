#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "rdma_transfer.h"

#define RDMA_PORT 7471
#define RDMA_CHUNK_SIZE (1024 * 1024)

static int ensure_test_file_exists(const char *path) {
    if (access(path, F_OK) == 0) {
        return 0;
    }

    printf("Creating 1GB test file...\n");
    if (system("dd if=/dev/zero of=test_1gb.bin bs=1M count=1024 status=none") != 0) {
        fprintf(stderr, "Failed to create test file\n");
        return -1;
    }

    return 0;
}

int main(void) {
    const char *test_file = "test_1gb.bin";
    char server_ip[64];
    rdma_benchmark_result_t result;

    printf("=== Real RDMA Performance Test ===\n");
    printf("Transfer mode: RDMA_WRITE over rdma_cm/libibverbs\n");
    printf("File size: 1GB\n");

    if (ensure_test_file_exists(test_file) != 0) {
        return 1;
    }

    if (rdma_detect_local_ip(server_ip, sizeof(server_ip)) != 0) {
        fprintf(stderr, "Failed to detect a usable RDMA IP. Set RDMA_IP manually.\n");
        return 1;
    }

    printf("Using RDMA IP: %s\n", server_ip);

    if (run_rdma_file_write_benchmark(server_ip, RDMA_PORT, test_file,
                                      RDMA_CHUNK_SIZE, 1, &result) != 0) {
        fprintf(stderr, "Real RDMA benchmark failed\n");
        return 1;
    }

    printf("\n=== Test Complete ===\n");
    printf("Transferred: %.2f MB\n", result.bytes_transferred / (1024.0 * 1024.0));
    printf("RDMA time: %.2f ms\n", result.elapsed_ms);
    printf("RDMA speed: %.2f MB/s\n",
           (result.bytes_transferred / (1024.0 * 1024.0)) /
               (result.elapsed_ms / 1000.0));

    return 0;
}