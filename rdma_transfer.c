#include "rdma_transfer.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <infiniband/verbs.h>
#include <net/if.h>
#include <poll.h>
#include <pthread.h>
#include <rdma/rdma_cma.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#define RDMA_CM_TIMEOUT_MS 120000
#define RDMA_WC_TIMEOUT_MS 120000

enum control_message_type {
    CONTROL_BUFFER_INFO = 1,
    CONTROL_DONE = 2,
};

typedef struct {
    uint32_t type;
    uint32_t rkey;
    uint32_t length;
    uint64_t addr;
    uint64_t total_bytes;
} control_message_t;

typedef struct {
    struct rdma_event_channel *event_channel;
    struct rdma_cm_id *id;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    char *data_buffer;
    size_t data_buffer_size;
    struct ibv_mr *data_mr;
    control_message_t *send_message;
    control_message_t *recv_message;
    struct ibv_mr *send_mr;
    struct ibv_mr *recv_mr;
} rdma_endpoint_t;

typedef struct {
    const char *server_ip;
    uint16_t port;
    size_t chunk_size;
    int verbose;
    int status;
    size_t bytes_transferred;
} rdma_server_args_t;

static double get_time_ms(void) {
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

static int report_error(int verbose, const char *message) {
    if (verbose) {
        perror(message);
    }
    return -1;
}

static int get_wc_timeout_ms(void) {
    const char *timeout_env = getenv("RDMA_WC_TIMEOUT_MS");
    char *end_ptr = NULL;
    long timeout_ms;

    if (timeout_env == NULL || timeout_env[0] == '\0') {
        return RDMA_WC_TIMEOUT_MS;
    }

    timeout_ms = strtol(timeout_env, &end_ptr, 10);
    if (end_ptr == timeout_env || *end_ptr != '\0' || timeout_ms <= 0 ||
        timeout_ms > INT32_MAX) {
        return RDMA_WC_TIMEOUT_MS;
    }

    return (int)timeout_ms;
}

static int get_cm_timeout_ms(void) {
    const char *timeout_env = getenv("RDMA_CM_TIMEOUT_MS");
    char *end_ptr = NULL;
    long timeout_ms;

    if (timeout_env == NULL || timeout_env[0] == '\0') {
        return RDMA_CM_TIMEOUT_MS;
    }

    timeout_ms = strtol(timeout_env, &end_ptr, 10);
    if (end_ptr == timeout_env || *end_ptr != '\0' || timeout_ms <= 0 ||
        timeout_ms > INT32_MAX) {
        return RDMA_CM_TIMEOUT_MS;
    }

    return (int)timeout_ms;
}

static int wait_for_cm_event(struct rdma_event_channel *channel,
                             enum rdma_cm_event_type expected_type,
                             struct rdma_cm_event **event_out,
                             int verbose) {
    struct rdma_cm_event *event;
    struct pollfd poll_fd;
    int cm_timeout_ms = get_cm_timeout_ms();
    int poll_result;

    poll_fd.fd = channel->fd;
    poll_fd.events = POLLIN;
    poll_fd.revents = 0;

    poll_result = poll(&poll_fd, 1, cm_timeout_ms);
    if (poll_result < 0) {
        return report_error(verbose, "poll(rdma cm event)");
    }

    if (poll_result == 0) {
        if (verbose) {
            fprintf(stderr,
                    "Timed out waiting for RDMA CM event %d after %d ms\n",
                    expected_type, cm_timeout_ms);
        }
        return -1;
    }

    if (rdma_get_cm_event(channel, &event) != 0) {
        return report_error(verbose, "rdma_get_cm_event");
    }

    if (event->event != expected_type) {
        if (verbose) {
            fprintf(stderr, "Unexpected RDMA CM event: expected %d, got %d\n",
                    expected_type, event->event);
        }
        rdma_ack_cm_event(event);
        return -1;
    }

    *event_out = event;
    return 0;
}

static int wait_for_wc(struct ibv_cq *cq, enum ibv_wc_opcode expected_opcode,
                       int verbose) {
    struct ibv_wc wc;
    double deadline_ms = get_time_ms() + get_wc_timeout_ms();

    while (get_time_ms() < deadline_ms) {
        int poll_result = ibv_poll_cq(cq, 1, &wc);

        if (poll_result < 0) {
            if (verbose) {
                fprintf(stderr, "ibv_poll_cq failed\n");
            }
            return -1;
        }

        if (poll_result == 0) {
            usleep(1000);
            continue;
        }

        if (wc.status != IBV_WC_SUCCESS) {
            if (verbose) {
                fprintf(stderr, "Work completion failed: %s\n",
                        ibv_wc_status_str(wc.status));
            }
            return -1;
        }

        if (wc.opcode != expected_opcode) {
            if (verbose) {
                fprintf(stderr, "Unexpected completion opcode: expected %d, got %d\n",
                        expected_opcode, wc.opcode);
            }
            return -1;
        }

        return 0;
    }

    if (verbose) {
        fprintf(stderr, "Timed out waiting for completion opcode %d\n",
                expected_opcode);
    }
    return -1;
}

static int post_receive_control(rdma_endpoint_t *endpoint, int verbose) {
    struct ibv_sge sge;
    struct ibv_recv_wr recv_wr;
    struct ibv_recv_wr *bad_wr = NULL;

    memset(endpoint->recv_message, 0, sizeof(*endpoint->recv_message));
    memset(&sge, 0, sizeof(sge));
    memset(&recv_wr, 0, sizeof(recv_wr));

    sge.addr = (uintptr_t)endpoint->recv_message;
    sge.length = sizeof(*endpoint->recv_message);
    sge.lkey = endpoint->recv_mr->lkey;

    recv_wr.sg_list = &sge;
    recv_wr.num_sge = 1;

    if (ibv_post_recv(endpoint->id->qp, &recv_wr, &bad_wr) != 0) {
        return report_error(verbose, "ibv_post_recv");
    }

    return 0;
}

static int post_send_control(rdma_endpoint_t *endpoint, int verbose) {
    struct ibv_sge sge;
    struct ibv_send_wr send_wr;
    struct ibv_send_wr *bad_wr = NULL;

    memset(&sge, 0, sizeof(sge));
    memset(&send_wr, 0, sizeof(send_wr));

    sge.addr = (uintptr_t)endpoint->send_message;
    sge.length = sizeof(*endpoint->send_message);
    sge.lkey = endpoint->send_mr->lkey;

    send_wr.opcode = IBV_WR_SEND;
    send_wr.send_flags = IBV_SEND_SIGNALED;
    send_wr.sg_list = &sge;
    send_wr.num_sge = 1;

    if (ibv_post_send(endpoint->id->qp, &send_wr, &bad_wr) != 0) {
        return report_error(verbose, "ibv_post_send(send)");
    }

    return 0;
}

static int post_rdma_write(rdma_endpoint_t *endpoint, uint64_t remote_addr,
                           uint32_t remote_rkey, size_t length, int verbose) {
    struct ibv_sge sge;
    struct ibv_send_wr send_wr;
    struct ibv_send_wr *bad_wr = NULL;

    memset(&sge, 0, sizeof(sge));
    memset(&send_wr, 0, sizeof(send_wr));

    sge.addr = (uintptr_t)endpoint->data_buffer;
    sge.length = (uint32_t)length;
    sge.lkey = endpoint->data_mr->lkey;

    send_wr.opcode = IBV_WR_RDMA_WRITE;
    send_wr.send_flags = IBV_SEND_SIGNALED;
    send_wr.sg_list = &sge;
    send_wr.num_sge = 1;
    send_wr.wr.rdma.remote_addr = remote_addr;
    send_wr.wr.rdma.rkey = remote_rkey;

    if (ibv_post_send(endpoint->id->qp, &send_wr, &bad_wr) != 0) {
        return report_error(verbose, "ibv_post_send(rdma_write)");
    }

    return 0;
}

static void cleanup_endpoint(rdma_endpoint_t *endpoint) {
    if (endpoint->id != NULL && endpoint->id->qp != NULL) {
        rdma_destroy_qp(endpoint->id);
    }
    if (endpoint->data_mr != NULL) {
        ibv_dereg_mr(endpoint->data_mr);
    }
    if (endpoint->send_mr != NULL) {
        ibv_dereg_mr(endpoint->send_mr);
    }
    if (endpoint->recv_mr != NULL) {
        ibv_dereg_mr(endpoint->recv_mr);
    }
    if (endpoint->data_buffer != NULL) {
        free(endpoint->data_buffer);
    }
    if (endpoint->send_message != NULL) {
        free(endpoint->send_message);
    }
    if (endpoint->recv_message != NULL) {
        free(endpoint->recv_message);
    }
    if (endpoint->cq != NULL) {
        ibv_destroy_cq(endpoint->cq);
    }
    if (endpoint->pd != NULL) {
        ibv_dealloc_pd(endpoint->pd);
    }
    if (endpoint->id != NULL) {
        rdma_destroy_id(endpoint->id);
    }
    if (endpoint->event_channel != NULL) {
        rdma_destroy_event_channel(endpoint->event_channel);
    }
}

static int init_endpoint(rdma_endpoint_t *endpoint, struct rdma_cm_id *id,
                         size_t data_buffer_size, int access_flags,
                         int verbose) {
    struct ibv_qp_init_attr qp_attr;

    endpoint->id = id;
    endpoint->data_buffer_size = data_buffer_size;

    endpoint->pd = ibv_alloc_pd(id->verbs);
    if (endpoint->pd == NULL) {
        return report_error(verbose, "ibv_alloc_pd");
    }

    endpoint->cq = ibv_create_cq(id->verbs, 16, NULL, NULL, 0);
    if (endpoint->cq == NULL) {
        return report_error(verbose, "ibv_create_cq");
    }

    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.send_cq = endpoint->cq;
    qp_attr.recv_cq = endpoint->cq;
    qp_attr.qp_type = IBV_QPT_RC;
    qp_attr.cap.max_send_wr = 16;
    qp_attr.cap.max_recv_wr = 16;
    qp_attr.cap.max_send_sge = 1;
    qp_attr.cap.max_recv_sge = 1;

    if (rdma_create_qp(id, endpoint->pd, &qp_attr) != 0) {
        return report_error(verbose, "rdma_create_qp");
    }

    endpoint->data_buffer = aligned_alloc(4096, data_buffer_size);
    if (endpoint->data_buffer == NULL) {
        return report_error(verbose, "aligned_alloc(data_buffer)");
    }

    endpoint->send_message = calloc(1, sizeof(*endpoint->send_message));
    endpoint->recv_message = calloc(1, sizeof(*endpoint->recv_message));
    if (endpoint->send_message == NULL || endpoint->recv_message == NULL) {
        return report_error(verbose, "calloc(control_message)");
    }

    endpoint->data_mr = ibv_reg_mr(endpoint->pd, endpoint->data_buffer,
                                   endpoint->data_buffer_size, access_flags);
    if (endpoint->data_mr == NULL) {
        return report_error(verbose, "ibv_reg_mr(data)");
    }

    endpoint->send_mr = ibv_reg_mr(endpoint->pd, endpoint->send_message,
                                   sizeof(*endpoint->send_message),
                                   IBV_ACCESS_LOCAL_WRITE);
    endpoint->recv_mr = ibv_reg_mr(endpoint->pd, endpoint->recv_message,
                                   sizeof(*endpoint->recv_message),
                                   IBV_ACCESS_LOCAL_WRITE);
    if (endpoint->send_mr == NULL || endpoint->recv_mr == NULL) {
        return report_error(verbose, "ibv_reg_mr(control)");
    }

    return 0;
}

static void *server_thread_main(void *arg) {
    rdma_server_args_t *server_args = (rdma_server_args_t *)arg;
    rdma_endpoint_t endpoint;
    struct rdma_cm_id *listen_id = NULL;
    struct rdma_cm_event *event = NULL;
    struct sockaddr_in server_addr;
    struct rdma_conn_param conn_param;

    memset(&endpoint, 0, sizeof(endpoint));

    endpoint.event_channel = rdma_create_event_channel();
    if (endpoint.event_channel == NULL) {
        server_args->status = report_error(server_args->verbose,
                                           "rdma_create_event_channel(server)");
        return NULL;
    }

    if (rdma_create_id(endpoint.event_channel, &listen_id, NULL,
                       RDMA_PS_TCP) != 0) {
        server_args->status = report_error(server_args->verbose,
                                           "rdma_create_id(listen)");
        cleanup_endpoint(&endpoint);
        return NULL;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_args->port);
    if (inet_pton(AF_INET, server_args->server_ip, &server_addr.sin_addr) != 1) {
        if (server_args->verbose) {
            fprintf(stderr, "Invalid RDMA server IP: %s\n", server_args->server_ip);
        }
        rdma_destroy_id(listen_id);
        cleanup_endpoint(&endpoint);
        server_args->status = -1;
        return NULL;
    }

    if (rdma_bind_addr(listen_id, (struct sockaddr *)&server_addr) != 0) {
        server_args->status = report_error(server_args->verbose, "rdma_bind_addr");
        rdma_destroy_id(listen_id);
        cleanup_endpoint(&endpoint);
        return NULL;
    }

    if (rdma_listen(listen_id, 1) != 0) {
        server_args->status = report_error(server_args->verbose, "rdma_listen");
        rdma_destroy_id(listen_id);
        cleanup_endpoint(&endpoint);
        return NULL;
    }

    if (wait_for_cm_event(endpoint.event_channel, RDMA_CM_EVENT_CONNECT_REQUEST,
                          &event, server_args->verbose) != 0) {
        rdma_destroy_id(listen_id);
        cleanup_endpoint(&endpoint);
        server_args->status = -1;
        return NULL;
    }

    endpoint.id = event->id;
    rdma_ack_cm_event(event);

    if (init_endpoint(&endpoint, endpoint.id, server_args->chunk_size,
                      IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE,
                      server_args->verbose) != 0) {
        rdma_destroy_id(listen_id);
        cleanup_endpoint(&endpoint);
        server_args->status = -1;
        return NULL;
    }

    if (post_receive_control(&endpoint, server_args->verbose) != 0) {
        rdma_destroy_id(listen_id);
        cleanup_endpoint(&endpoint);
        server_args->status = -1;
        return NULL;
    }

    memset(&conn_param, 0, sizeof(conn_param));
    conn_param.initiator_depth = 1;
    conn_param.responder_resources = 1;
    conn_param.rnr_retry_count = 7;

    if (rdma_accept(endpoint.id, &conn_param) != 0) {
        server_args->status = report_error(server_args->verbose, "rdma_accept");
        rdma_destroy_id(listen_id);
        cleanup_endpoint(&endpoint);
        return NULL;
    }

    if (wait_for_cm_event(endpoint.event_channel, RDMA_CM_EVENT_ESTABLISHED,
                          &event, server_args->verbose) != 0) {
        rdma_destroy_id(listen_id);
        cleanup_endpoint(&endpoint);
        server_args->status = -1;
        return NULL;
    }
    rdma_ack_cm_event(event);

    endpoint.send_message->type = CONTROL_BUFFER_INFO;
    endpoint.send_message->rkey = endpoint.data_mr->rkey;
    endpoint.send_message->length = (uint32_t)endpoint.data_buffer_size;
    endpoint.send_message->addr = (uint64_t)(uintptr_t)endpoint.data_buffer;
    endpoint.send_message->total_bytes = 0;

    if (post_send_control(&endpoint, server_args->verbose) != 0 ||
        wait_for_wc(endpoint.cq, IBV_WC_SEND, server_args->verbose) != 0 ||
        wait_for_wc(endpoint.cq, IBV_WC_RECV, server_args->verbose) != 0) {
        rdma_destroy_id(listen_id);
        cleanup_endpoint(&endpoint);
        server_args->status = -1;
        return NULL;
    }

    if (endpoint.recv_message->type != CONTROL_DONE) {
        if (server_args->verbose) {
            fprintf(stderr, "Unexpected control message type from client: %u\n",
                    endpoint.recv_message->type);
        }
        rdma_destroy_id(listen_id);
        cleanup_endpoint(&endpoint);
        server_args->status = -1;
        return NULL;
    }

    server_args->bytes_transferred = (size_t)endpoint.recv_message->total_bytes;

    if (rdma_disconnect(endpoint.id) != 0) {
        server_args->status = report_error(server_args->verbose, "rdma_disconnect(server)");
        rdma_destroy_id(listen_id);
        cleanup_endpoint(&endpoint);
        return NULL;
    }

    if (wait_for_cm_event(endpoint.event_channel, RDMA_CM_EVENT_DISCONNECTED,
                          &event, server_args->verbose) != 0) {
        rdma_destroy_id(listen_id);
        cleanup_endpoint(&endpoint);
        server_args->status = -1;
        return NULL;
    }
    rdma_ack_cm_event(event);

    rdma_destroy_id(listen_id);
    cleanup_endpoint(&endpoint);
    server_args->status = 0;
    return NULL;
}

int run_rdma_file_write_client(const char *server_ip, uint16_t port,
                               const char *filename, size_t chunk_size,
                               int verbose,
                               rdma_benchmark_result_t *result) {
    rdma_endpoint_t endpoint;
    struct rdma_cm_event *event = NULL;
    struct sockaddr_in server_addr;
    struct rdma_conn_param conn_param;
    uint64_t remote_addr;
    uint32_t remote_rkey;
    int file_fd = -1;
    ssize_t bytes_read;
    size_t total_bytes = 0;
    double start_time_ms;
    double end_time_ms;

    memset(&endpoint, 0, sizeof(endpoint));

    endpoint.event_channel = rdma_create_event_channel();
    if (endpoint.event_channel == NULL) {
        return report_error(verbose, "rdma_create_event_channel(client)");
    }

    if (rdma_create_id(endpoint.event_channel, &endpoint.id, NULL,
                       RDMA_PS_TCP) != 0) {
        cleanup_endpoint(&endpoint);
        return report_error(verbose, "rdma_create_id(client)");
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) != 1) {
        if (verbose) {
            fprintf(stderr, "Invalid RDMA server IP: %s\n", server_ip);
        }
        cleanup_endpoint(&endpoint);
        return -1;
    }

    if (rdma_resolve_addr(endpoint.id, NULL, (struct sockaddr *)&server_addr,
                          get_cm_timeout_ms()) != 0) {
        cleanup_endpoint(&endpoint);
        return report_error(verbose, "rdma_resolve_addr");
    }

    if (wait_for_cm_event(endpoint.event_channel, RDMA_CM_EVENT_ADDR_RESOLVED,
                          &event, verbose) != 0) {
        cleanup_endpoint(&endpoint);
        return -1;
    }
    rdma_ack_cm_event(event);

    if (rdma_resolve_route(endpoint.id, get_cm_timeout_ms()) != 0) {
        cleanup_endpoint(&endpoint);
        return report_error(verbose, "rdma_resolve_route");
    }

    if (wait_for_cm_event(endpoint.event_channel, RDMA_CM_EVENT_ROUTE_RESOLVED,
                          &event, verbose) != 0) {
        cleanup_endpoint(&endpoint);
        return -1;
    }
    rdma_ack_cm_event(event);

    if (init_endpoint(&endpoint, endpoint.id, chunk_size,
                      IBV_ACCESS_LOCAL_WRITE, verbose) != 0) {
        cleanup_endpoint(&endpoint);
        return -1;
    }

    if (post_receive_control(&endpoint, verbose) != 0) {
        cleanup_endpoint(&endpoint);
        return -1;
    }

    memset(&conn_param, 0, sizeof(conn_param));
    conn_param.initiator_depth = 1;
    conn_param.responder_resources = 1;
    conn_param.retry_count = 7;
    conn_param.rnr_retry_count = 7;

    if (rdma_connect(endpoint.id, &conn_param) != 0) {
        cleanup_endpoint(&endpoint);
        return report_error(verbose, "rdma_connect");
    }

    if (wait_for_cm_event(endpoint.event_channel, RDMA_CM_EVENT_ESTABLISHED,
                          &event, verbose) != 0) {
        cleanup_endpoint(&endpoint);
        return -1;
    }
    rdma_ack_cm_event(event);

    if (wait_for_wc(endpoint.cq, IBV_WC_RECV, verbose) != 0) {
        cleanup_endpoint(&endpoint);
        return -1;
    }

    if (endpoint.recv_message->type != CONTROL_BUFFER_INFO) {
        if (verbose) {
            fprintf(stderr, "Unexpected control message type from server: %u\n",
                    endpoint.recv_message->type);
        }
        cleanup_endpoint(&endpoint);
        return -1;
    }

    remote_addr = endpoint.recv_message->addr;
    remote_rkey = endpoint.recv_message->rkey;

    file_fd = open(filename, O_RDONLY);
    if (file_fd < 0) {
        cleanup_endpoint(&endpoint);
        return report_error(verbose, "open(test file)");
    }

    start_time_ms = get_time_ms();

    while ((bytes_read = read(file_fd, endpoint.data_buffer, chunk_size)) > 0) {
        if (post_rdma_write(&endpoint, remote_addr, remote_rkey,
                            (size_t)bytes_read, verbose) != 0 ||
            wait_for_wc(endpoint.cq, IBV_WC_RDMA_WRITE, verbose) != 0) {
            close(file_fd);
            cleanup_endpoint(&endpoint);
            return -1;
        }
        total_bytes += (size_t)bytes_read;
    }

    if (bytes_read < 0) {
        close(file_fd);
        cleanup_endpoint(&endpoint);
        return report_error(verbose, "read(test file)");
    }

    endpoint.send_message->type = CONTROL_DONE;
    endpoint.send_message->rkey = 0;
    endpoint.send_message->length = 0;
    endpoint.send_message->addr = 0;
    endpoint.send_message->total_bytes = total_bytes;

    if (post_send_control(&endpoint, verbose) != 0 ||
        wait_for_wc(endpoint.cq, IBV_WC_SEND, verbose) != 0) {
        close(file_fd);
        cleanup_endpoint(&endpoint);
        return -1;
    }

    if (wait_for_cm_event(endpoint.event_channel, RDMA_CM_EVENT_DISCONNECTED,
                          &event, verbose) != 0) {
        close(file_fd);
        cleanup_endpoint(&endpoint);
        return -1;
    }
    rdma_ack_cm_event(event);

    end_time_ms = get_time_ms();

    close(file_fd);
    cleanup_endpoint(&endpoint);

    result->elapsed_ms = end_time_ms - start_time_ms;
    result->bytes_transferred = total_bytes;
    return 0;
}

int rdma_detect_local_ip(char *buffer, size_t buffer_len) {
    const char *env_ip = getenv("RDMA_IP");
    struct ifaddrs *ifaddr = NULL;
    struct ifaddrs *ifa;

    if (env_ip != NULL && env_ip[0] != '\0') {
        if (snprintf(buffer, buffer_len, "%s", env_ip) < (int)buffer_len) {
            return 0;
        }
        return -1;
    }

    if (getifaddrs(&ifaddr) != 0) {
        return -1;
    }

    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL || ifa->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        if ((ifa->ifa_flags & IFF_LOOPBACK) != 0) {
            continue;
        }
        if ((ifa->ifa_flags & IFF_UP) == 0) {
            continue;
        }
        if (inet_ntop(AF_INET,
                      &((struct sockaddr_in *)ifa->ifa_addr)->sin_addr,
                      buffer, buffer_len) != NULL) {
            freeifaddrs(ifaddr);
            return 0;
        }
    }

    freeifaddrs(ifaddr);
    return -1;
}

int run_rdma_file_write_server(const char *bind_ip, uint16_t port,
                               size_t chunk_size, int verbose,
                               rdma_benchmark_result_t *result) {
    rdma_server_args_t server_args;

    memset(result, 0, sizeof(*result));
    memset(&server_args, 0, sizeof(server_args));

    server_args.server_ip = bind_ip;
    server_args.port = port;
    server_args.chunk_size = chunk_size;
    server_args.verbose = verbose;
    server_args.status = -1;

    server_thread_main(&server_args);
    if (server_args.status != 0) {
        return -1;
    }

    result->elapsed_ms = 0.0;
    result->bytes_transferred = server_args.bytes_transferred;
    return 0;
}

int run_rdma_file_write_benchmark(const char *server_ip, uint16_t port,
                                  const char *filename, size_t chunk_size,
                                  int verbose,
                                  rdma_benchmark_result_t *result) {
    pthread_t server_thread;
    rdma_server_args_t server_args;
    int thread_result;

    memset(result, 0, sizeof(*result));
    memset(&server_args, 0, sizeof(server_args));

    server_args.server_ip = server_ip;
    server_args.port = port;
    server_args.chunk_size = chunk_size;
    server_args.verbose = verbose;
    server_args.status = -1;

    if (pthread_create(&server_thread, NULL, server_thread_main, &server_args) != 0) {
        return report_error(verbose, "pthread_create(rdma server)");
    }

    usleep(200000);
    thread_result = run_rdma_file_write_client(server_ip, port, filename,
                                               chunk_size, verbose, result);
    pthread_join(server_thread, NULL);

    if (thread_result != 0 || server_args.status != 0) {
        return -1;
    }

    if (result->bytes_transferred != server_args.bytes_transferred) {
        if (verbose) {
            fprintf(stderr, "RDMA byte count mismatch: client=%zu server=%zu\n",
                    result->bytes_transferred, server_args.bytes_transferred);
        }
        return -1;
    }

    return 0;
}