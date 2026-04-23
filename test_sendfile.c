#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/sendfile.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <pthread.h>

#define PORT 9999
#define FILE_SIZE (1024 * 1024 * 1024)  // 1GB

typedef struct {
    size_t expected_bytes;
    size_t received_bytes;
    int status;
} receiver_args_t;

double get_time_ms() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

// 服务器线程
void* server_thread(void* arg) {
    receiver_args_t* receiver_args = (receiver_args_t*)arg;
    int server_fd, client_fd;
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    char buffer[64 * 1024];
    ssize_t bytes_received;
    
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("Socket creation failed");
        receiver_args->status = -1;
        return NULL;
    }
    
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);
    
    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("Bind failed");
        close(server_fd);
        receiver_args->status = -1;
        return NULL;
    }
    
    if (listen(server_fd, 1) < 0) {
        perror("Listen failed");
        close(server_fd);
        receiver_args->status = -1;
        return NULL;
    }
    
    client_fd = accept(server_fd, (struct sockaddr*)&addr, &addr_len);
    if (client_fd < 0) {
        perror("Accept failed");
        close(server_fd);
        receiver_args->status = -1;
        return NULL;
    }
    
    receiver_args->received_bytes = 0;
    while (receiver_args->received_bytes < receiver_args->expected_bytes) {
        bytes_received = recv(client_fd, buffer, sizeof(buffer), 0);
        if (bytes_received <= 0) {
            break;
        }
        receiver_args->received_bytes += (size_t)bytes_received;
    }
    
    receiver_args->status =
        receiver_args->received_bytes == receiver_args->expected_bytes ? 0 : -1;

    close(client_fd);
    close(server_fd);
    return NULL;
}

double test_sendfile() {
    int file_fd, sock_fd;
    struct stat st;
    double start_time, end_time;
    pthread_t thread;
    receiver_args_t receiver_args;
    
    printf("\n=== Sendfile Test ===\n");
    
    // 打开文件
    file_fd = open("test_1gb.bin", O_RDONLY);
    if (file_fd < 0) {
        perror("Failed to open file");
        return -1;
    }
    
    if (fstat(file_fd, &st) != 0) {
        perror("Failed to get file stats");
        close(file_fd);
        return -1;
    }
    
    printf("File size: %.2f GB\n", st.st_size / (1024.0 * 1024.0 * 1024.0));
    
    // 启动服务器线程
    receiver_args.expected_bytes = (size_t)st.st_size;
    receiver_args.received_bytes = 0;
    receiver_args.status = -1;
    pthread_create(&thread, NULL, server_thread, &receiver_args);
    sleep(1);  // 等待服务器启动
    
    // 客户端连接
    sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    
    if (connect(sock_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Connection failed");
        close(file_fd);
        return -1;
    }
    
    start_time = get_time_ms();
    
    // 使用sendfile传输文件
    off_t offset = 0;
    ssize_t bytes_sent;
    while (offset < st.st_size) {
        bytes_sent = sendfile(sock_fd, file_fd, &offset,
                             st.st_size - offset);
        if (bytes_sent > 0) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            usleep(1000);
            continue;
        }
        break;
    }

    shutdown(sock_fd, SHUT_WR);
    pthread_join(thread, NULL);
    
    end_time = get_time_ms();
    
    printf("Sendfile transfer completed\n");
    printf("Sendfile time: %.2f ms\n", end_time - start_time);
    printf("Sendfile speed: %.2f MB/s\n", 
           (st.st_size / (1024.0 * 1024.0)) / ((end_time - start_time) / 1000.0));
    
    // 清理
    close(sock_fd);
    close(file_fd);

    if (offset != st.st_size || receiver_args.status != 0) {
        fprintf(stderr, "Sendfile transfer was incomplete\n");
        return -1;
    }
    
    return end_time - start_time;
}

int main() {
    double sendfile_time;
    
    printf("=== Sendfile Performance Test ===\n");
    printf("Testing 1GB file transfer using sendfile\n");
    
    // 创建测试文件（如果不存在）
    if (access("test_1gb.bin", F_OK) != 0) {
        printf("Creating 1GB test file...\n");
        int create_result = system("dd if=/dev/zero of=test_1gb.bin bs=1M count=1024 2>/dev/null");
        if (create_result != 0) {
            fprintf(stderr, "Failed to create test file\n");
            return 1;
        }
    }
    
    // 测试sendfile
    sendfile_time = test_sendfile();
    
    if (sendfile_time > 0) {
        printf("\n=== Test Complete ===\n");
        printf("Total time: %.2f ms\n", sendfile_time);
    }
    
    return 0;
}