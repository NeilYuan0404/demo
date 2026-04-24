#include <arpa/inet.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "rdma_transfer.h"

#define PORT 9999
#define RDMA_PORT 7471
#define DEFAULT_FILE "test_1gb.bin"
#define BUFFER_SIZE (1024 * 1024)
#define TRANSFER_MAGIC 0x54524652u
#define TCP_CONNECT_RETRY_COUNT 50
#define TCP_CONNECT_RETRY_DELAY_US 100000

typedef enum {
    TRANSFER_MODE_SENDFILE = 1,
    TRANSFER_MODE_TRADITIONAL = 2,
} transfer_mode_t;

typedef struct {
    uint32_t magic;
    uint32_t mode;
    uint64_t total_bytes;
} transfer_header_t;

typedef struct {
    uint32_t magic;
    uint32_t status;
    uint64_t received_bytes;
} transfer_ack_t;

static double get_time_ms(void) {
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

static int ensure_test_file_exists(const char *path) {
    if (access(path, F_OK) == 0) {
        return 0;
    }

    printf("\nCreating 1GB test file...\n");
    if (system("dd if=/dev/zero of=test_1gb.bin bs=1M count=1024 status=none") != 0) {
        fprintf(stderr, "Failed to create test file\n");
        return -1;
    }

    printf("Test file created successfully\n");
    return 0;
}

static int send_all(int fd, const void *buffer, size_t length) {
    const char *cursor = (const char *)buffer;
    size_t total_sent = 0;

    while (total_sent < length) {
        ssize_t bytes_sent = send(fd, cursor + total_sent, length - total_sent, 0);
        if (bytes_sent <= 0) {
            return -1;
        }
        total_sent += (size_t)bytes_sent;
    }

    return 0;
}

static int recv_all(int fd, void *buffer, size_t length) {
    char *cursor = (char *)buffer;
    size_t total_received = 0;

    while (total_received < length) {
        ssize_t bytes_received = recv(fd, cursor + total_received,
                                      length - total_received, 0);
        if (bytes_received <= 0) {
            return -1;
        }
        total_received += (size_t)bytes_received;
    }

    return 0;
}

static int connect_to_server(const char *server_ip, uint16_t port) {
    int sock_fd;
    struct sockaddr_in server_addr;
    int attempt;

    sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("socket");
        return -1;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) != 1) {
        fprintf(stderr, "Invalid server IP: %s\n", server_ip);
        close(sock_fd);
        return -1;
    }

    for (attempt = 0; attempt < TCP_CONNECT_RETRY_COUNT; ++attempt) {
        if (connect(sock_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == 0) {
            return sock_fd;
        }

        if (errno != ECONNREFUSED && errno != EINTR) {
            perror("connect");
            close(sock_fd);
            return -1;
        }

        if (attempt + 1 < TCP_CONNECT_RETRY_COUNT) {
            usleep(TCP_CONNECT_RETRY_DELAY_US);
        }
    }

    perror("connect");
    close(sock_fd);
    return -1;
}

static int send_transfer_header(int sock_fd, transfer_mode_t mode,
                                size_t total_bytes) {
    transfer_header_t header;

    header.magic = htonl(TRANSFER_MAGIC);
    header.mode = htonl((uint32_t)mode);
    header.total_bytes = htobe64((uint64_t)total_bytes);

    return send_all(sock_fd, &header, sizeof(header));
}

static int read_transfer_ack(int sock_fd, size_t expected_bytes) {
    transfer_ack_t ack;

    if (recv_all(sock_fd, &ack, sizeof(ack)) != 0) {
        return -1;
    }

    if (ntohl(ack.magic) != TRANSFER_MAGIC || ntohl(ack.status) != 0) {
        return -1;
    }

    return (size_t)be64toh(ack.received_bytes) == expected_bytes ? 0 : -1;
}

static const char *transfer_mode_name(transfer_mode_t mode) {
    switch (mode) {
    case TRANSFER_MODE_SENDFILE:
        return "sendfile";
    case TRANSFER_MODE_TRADITIONAL:
        return "traditional";
    default:
        return "unknown";
    }
}

static int run_tcp_validation_server(const char *bind_ip, uint16_t port,
                                     int expected_connections) {
    int server_fd;
    int opt = 1;
    int connection_index;
    struct sockaddr_in addr;
    char buffer[BUFFER_SIZE];

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket(tcp server)");
        return -1;
    }

    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (strcmp(bind_ip, "0.0.0.0") == 0) {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else if (inet_pton(AF_INET, bind_ip, &addr.sin_addr) != 1) {
        fprintf(stderr, "Invalid bind IP: %s\n", bind_ip);
        close(server_fd);
        return -1;
    }

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(server_fd, expected_connections) != 0) {
        perror("bind/listen(tcp server)");
        close(server_fd);
        return -1;
    }

    printf("TCP receiver listening on %s:%u\n", bind_ip, port);

    for (connection_index = 0; connection_index < expected_connections;
         ++connection_index) {
        int client_fd;
        transfer_header_t header;
        transfer_ack_t ack;
        transfer_mode_t mode;
        size_t expected_bytes;
        size_t received_bytes = 0;

        client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) {
            perror("accept");
            close(server_fd);
            return -1;
        }

        if (recv_all(client_fd, &header, sizeof(header)) != 0 ||
            ntohl(header.magic) != TRANSFER_MAGIC) {
            close(client_fd);
            close(server_fd);
            return -1;
        }

        mode = (transfer_mode_t)ntohl(header.mode);
        expected_bytes = (size_t)be64toh(header.total_bytes);

        while (received_bytes < expected_bytes) {
            size_t remaining = expected_bytes - received_bytes;
            size_t chunk = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
            ssize_t bytes_received = recv(client_fd, buffer, chunk, 0);
            if (bytes_received <= 0) {
                break;
            }
            received_bytes += (size_t)bytes_received;
        }

        ack.magic = htonl(TRANSFER_MAGIC);
        ack.status = htonl(received_bytes == expected_bytes ? 0u : 1u);
        ack.received_bytes = htobe64((uint64_t)received_bytes);

        if (send_all(client_fd, &ack, sizeof(ack)) != 0) {
            close(client_fd);
            close(server_fd);
            return -1;
        }

        printf("Validated TCP %s transfer: %zu bytes\n",
               transfer_mode_name(mode), received_bytes);
        close(client_fd);

        if (received_bytes != expected_bytes) {
            close(server_fd);
            return -1;
        }
    }

    close(server_fd);
    return 0;
}

static double test_real_rdma(const char *server_ip, const char *filename) {
    rdma_benchmark_result_t result;

    printf("\n=== 1. Real RDMA Write ===\n");
    printf("RDMA server IP: %s\n", server_ip);

    if (run_rdma_file_write_client(server_ip, RDMA_PORT, filename,
                                   BUFFER_SIZE, 1, &result) != 0) {
        return -1;
    }

    printf("Time: %.2f ms\n", result.elapsed_ms);
    printf("Speed: %.2f MB/s\n",
           (result.bytes_transferred / (1024.0 * 1024.0)) /
               (result.elapsed_ms / 1000.0));
    return result.elapsed_ms;
}

static double test_sendfile(const char *server_ip, const char *filename) {
    int file_fd;
    int sock_fd;
    struct stat st;
    double start_time;
    double end_time;
    off_t offset = 0;

    printf("\n=== 2. Sendfile Transfer ===\n");

    file_fd = open(filename, O_RDONLY);
    if (file_fd < 0) {
        perror("open(sendfile input)");
        return -1;
    }

    if (fstat(file_fd, &st) != 0) {
        perror("fstat(sendfile input)");
        close(file_fd);
        return -1;
    }
    printf("File size: %.2f GB\n", st.st_size / (1024.0 * 1024.0 * 1024.0));

    sock_fd = connect_to_server(server_ip, PORT);
    if (sock_fd < 0) {
        close(file_fd);
        return -1;
    }

    if (send_transfer_header(sock_fd, TRANSFER_MODE_SENDFILE,
                             (size_t)st.st_size) != 0) {
        close(sock_fd);
        close(file_fd);
        return -1;
    }

    start_time = get_time_ms();

    while (offset < st.st_size) {
        ssize_t bytes_sent = sendfile(sock_fd, file_fd, &offset,
                                      (size_t)(st.st_size - offset));
        if (bytes_sent > 0) {
            continue;
        }
        if (bytes_sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            usleep(1000);
            continue;
        }
        break;
    }

    shutdown(sock_fd, SHUT_WR);

    if (offset != st.st_size || read_transfer_ack(sock_fd, (size_t)st.st_size) != 0) {
        close(sock_fd);
        close(file_fd);
        return -1;
    }

    end_time = get_time_ms();

    close(sock_fd);
    close(file_fd);

    printf("Time: %.2f ms\n", end_time - start_time);
    printf("Speed: %.2f MB/s\n",
           (st.st_size / (1024.0 * 1024.0)) / ((end_time - start_time) / 1000.0));
    return end_time - start_time;
}

static double test_traditional(const char *server_ip, const char *filename) {
    int src_fd;
    int sock_fd;
    char buffer[BUFFER_SIZE];
    ssize_t bytes_read;
    double start_time;
    double end_time;
    struct stat st;

    printf("\n=== 3. Traditional Read/Send ===\n");

    src_fd = open(filename, O_RDONLY);
    if (src_fd < 0) {
        perror("open(traditional input)");
        return -1;
    }

    if (fstat(src_fd, &st) != 0) {
        perror("fstat(traditional input)");
        close(src_fd);
        return -1;
    }
    printf("File size: %.2f GB\n", st.st_size / (1024.0 * 1024.0 * 1024.0));

    sock_fd = connect_to_server(server_ip, PORT);
    if (sock_fd < 0) {
        close(src_fd);
        return -1;
    }

    if (send_transfer_header(sock_fd, TRANSFER_MODE_TRADITIONAL,
                             (size_t)st.st_size) != 0) {
        close(sock_fd);
        close(src_fd);
        return -1;
    }

    start_time = get_time_ms();

    while ((bytes_read = read(src_fd, buffer, sizeof(buffer))) > 0) {
        ssize_t total_sent = 0;
        while (total_sent < bytes_read) {
            ssize_t bytes_sent = send(sock_fd, buffer + total_sent,
                                      (size_t)(bytes_read - total_sent), 0);
            if (bytes_sent <= 0) {
                close(sock_fd);
                close(src_fd);
                return -1;
            }
            total_sent += bytes_sent;
        }
    }

    shutdown(sock_fd, SHUT_WR);

    if (bytes_read < 0 || read_transfer_ack(sock_fd, (size_t)st.st_size) != 0) {
        close(sock_fd);
        close(src_fd);
        return -1;
    }

    end_time = get_time_ms();

    close(src_fd);
    close(sock_fd);

    printf("Time: %.2f ms\n", end_time - start_time);
    printf("Speed: %.2f MB/s\n",
           (st.st_size / (1024.0 * 1024.0)) / ((end_time - start_time) / 1000.0));
    return end_time - start_time;
}

static void print_summary(double rdma_time, double sendfile_time,
                          double traditional_time, size_t file_size) {
    printf("\n========================================\n");
    printf("Performance Summary\n");
    printf("========================================\n");

    if (rdma_time > 0) {
        printf("1. Real RDMA:         %.2f ms (%.2f MB/s)\n",
               rdma_time, (file_size / (1024.0 * 1024.0)) / (rdma_time / 1000.0));
    }
    if (sendfile_time > 0) {
        printf("2. Sendfile:          %.2f ms (%.2f MB/s)\n",
               sendfile_time,
               (file_size / (1024.0 * 1024.0)) / (sendfile_time / 1000.0));
    }
    if (traditional_time > 0) {
        printf("3. Traditional:       %.2f ms (%.2f MB/s)\n",
               traditional_time,
               (file_size / (1024.0 * 1024.0)) / (traditional_time / 1000.0));
    }

    if (rdma_time > 0 && sendfile_time > 0) {
        printf("\nPerformance Ratio:\n");
        printf("RDMA is %.2fx faster than Sendfile\n", sendfile_time / rdma_time);
        if (traditional_time > 0) {
            printf("RDMA is %.2fx faster than Traditional\n",
                   traditional_time / rdma_time);
            printf("Sendfile is %.2fx faster than Traditional\n",
                   traditional_time / sendfile_time);
        }
    }
}

static int run_receiver(const char *bind_ip) {
    rdma_benchmark_result_t rdma_result;

    printf("========================================\n");
    printf("Dual-machine receiver mode\n");
    printf("Bind IP: %s\n", bind_ip);
    printf("========================================\n");

    printf("\nWaiting for RDMA sender on %s:%d ...\n", bind_ip, RDMA_PORT);
    if (run_rdma_file_write_server(bind_ip, RDMA_PORT, BUFFER_SIZE, 1,
                                   &rdma_result) != 0) {
        fprintf(stderr, "RDMA receive failed\n");
        return 1;
    }
    printf("Validated RDMA transfer: %zu bytes\n", rdma_result.bytes_transferred);

    printf("\nWaiting for TCP sender tests on %s:%d ...\n", bind_ip, PORT);
    if (run_tcp_validation_server(bind_ip, PORT, 2) != 0) {
        fprintf(stderr, "TCP validation failed\n");
        return 1;
    }

    printf("\nReceiver validation completed successfully.\n");
    return 0;
}

static int run_sender(const char *server_ip) {
    double rdma_time;
    double sendfile_time;
    double traditional_time;
    struct stat st;

    printf("========================================\n");
    printf("Dual-machine transfer comparison\n");
    printf("Remote receiver: %s\n", server_ip);
    printf("File Size: 1GB\n");
    printf("========================================\n");

    if (ensure_test_file_exists(DEFAULT_FILE) != 0) {
        return 1;
    }

    if (stat(DEFAULT_FILE, &st) != 0) {
        perror("stat(test file)");
        return 1;
    }

    rdma_time = test_real_rdma(server_ip, DEFAULT_FILE);
    sendfile_time = test_sendfile(server_ip, DEFAULT_FILE);
    traditional_time = test_traditional(server_ip, DEFAULT_FILE);

    print_summary(rdma_time, sendfile_time, traditional_time, (size_t)st.st_size);
    return 0;
}

static void print_usage(const char *program_name) {
    printf("Usage:\n");
    printf("  %s receive <bind_ip>\n", program_name);
    printf("  %s send <receiver_ip>\n", program_name);
    printf("\nExamples:\n");
    printf("  %s receive 192.168.1.20\n", program_name);
    printf("  %s send 192.168.1.20\n", program_name);
}

int main(int argc, char **argv) {
    if (argc != 3) {
        print_usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "receive") == 0) {
        return run_receiver(argv[2]);
    }

    if (strcmp(argv[1], "send") == 0) {
        return run_sender(argv[2]);
    }

    print_usage(argv[0]);
    return 1;
}
