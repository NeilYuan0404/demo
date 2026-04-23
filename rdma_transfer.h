#ifndef RDMA_TRANSFER_H
#define RDMA_TRANSFER_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    double elapsed_ms;
    size_t bytes_transferred;
} rdma_benchmark_result_t;

int rdma_detect_local_ip(char *buffer, size_t buffer_len);

int run_rdma_file_write_server(const char *bind_ip, uint16_t port,
                               size_t chunk_size, int verbose,
                               rdma_benchmark_result_t *result);

int run_rdma_file_write_client(const char *server_ip, uint16_t port,
                               const char *filename, size_t chunk_size,
                               int verbose,
                               rdma_benchmark_result_t *result);

int run_rdma_file_write_benchmark(const char *server_ip, uint16_t port,
                                  const char *filename, size_t chunk_size,
                                  int verbose,
                                  rdma_benchmark_result_t *result);

#endif