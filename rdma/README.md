# RDMA Demo Directory

这个目录包含一组用于对比真实 RDMA 写入、TCP `sendfile` 和传统 `read`/`send` 发送方式的实验程序，以及配套的编译和本地调试脚本。

## 目录内容

| 文件 | 说明 |
| --- | --- |
| `rdma_transfer.c` / `rdma_transfer.h` | RDMA 传输核心实现，封装了基于 `rdma_cm` 和 `libibverbs` 的连接建立、内存注册、控制消息交换和 `RDMA_WRITE` 基准逻辑。 |
| `simulate_rdma.c` | 单机 RDMA 基准入口。自动检测本机可用 RDMA IP，生成 `test_1gb.bin` 后调用 `run_rdma_file_write_benchmark()` 跑一轮真实 RDMA 写入测试。 |
| `compare_transfer.c` | 双机对比程序。支持 `receive <bind_ip>` 和 `send <receiver_ip>` 两种模式，发送端会依次测试真实 RDMA、TCP `sendfile` 和传统 `read`/`send`。 |
| `test_sendfile.c` | 纯本地回环的 `sendfile` 性能测试，不依赖 RDMA 设备，适合快速验证 TCP 零拷贝路径。 |
| `run_test.sh` | 一次性编译 `simulate_rdma`、`test_sendfile` 和 `compare_transfer` 三个可执行文件。 |
| `local_debug.sh` | 本地调试脚本。默认使用 `-O0 -g` 编译，并支持小文件快速调试：`sendfile`、`simulate`、`compare-local` 三种模式。 |
| `create_1gb_file.sh` | 生成带块标记和校验文件的 `test_1gb.bin`，便于验证传输后的内容完整性。 |
| `simulate_rdma` / `compare_transfer` / `test_sendfile` | 编译产物，可重新生成，不建议手工维护。 |

## 依赖

本目录代码默认使用 `gcc` 编译，依赖如下：

- `gcc`
- `pthread`
- `librdmacm`
- `libibverbs`
- 常见系统工具：`dd`、`truncate`、`sha256sum`

在 Debian/Ubuntu 系统上通常可以这样安装：

```bash
sudo apt-get install build-essential librdmacm-dev libibverbs-dev rdma-core
```

## 编译

编译全部测试程序：

```bash
cd /home/gentleyuan/demo/rdma
./run_test.sh
```

如果只想快速本地调试，直接使用：

```bash
cd /home/gentleyuan/demo/rdma
./local_debug.sh sendfile 64
```

这个入口会自动：

- 使用 `-O0 -g -Wall -Wextra` 重新编译目标程序
- 生成一个较小的测试文件，避免每次都处理 1 GiB 文件
- 立即执行对应测试，方便配合 `gdb` 或打印日志调试

## 运行方式

### 1. 本地回环调试

不需要 RDMA 硬件：

```bash
./local_debug.sh sendfile 32
```

这会编译并运行 `test_sendfile`，使用 32 MiB 本地测试文件走 TCP 回环。

### 2. 单机 RDMA 基准

需要本机存在可用 RDMA 网络配置：

```bash
./local_debug.sh simulate 64
```

或者直接运行：

```bash
./simulate_rdma
```

程序会自动探测本机可用 RDMA IP，并运行一轮真实 `RDMA_WRITE` 测试。

### 3. 双机对比测试

接收端机器：

```bash
./compare_transfer receive <bind_ip>
```

发送端机器：

```bash
./compare_transfer send <receiver_ip>
```

发送端会依次执行：

1. 真实 RDMA 写入
2. TCP `sendfile`
3. 传统 `read` + `send`

最后输出三种方式的耗时、吞吐和倍率对比。

### 4. 本地伪双端对比

如果只是验证 `compare_transfer` 的流程，可以在一台机器上跑：

```bash
RDMA_IP=192.168.1.20 ./local_debug.sh compare-local 64
```

这个模式会先在后台启动 `compare_transfer receive`，然后前台启动 `compare_transfer send`。这里不能使用 `127.0.0.1` 或 `localhost`，并且 `bind_ip` 必须是当前机器已经配置的非回环 IPv4 地址；真实 RDMA 部分仍然要求本机 RDMA 环境可用。

## 测试文件

默认测试文件名是 `test_1gb.bin`。

- `simulate_rdma.c`、`compare_transfer.c`、`test_sendfile.c` 在文件不存在时会尝试自动创建
- `create_1gb_file.sh` 会生成带块标记和 `.sha256` 校验文件的版本，更适合内容校验
- `local_debug.sh` 默认会覆盖同名文件，并根据参数生成更小的测试文件

生成带校验信息的测试文件：

```bash
./create_1gb_file.sh
sha256sum -c test_1gb.bin.sha256
```

## 调试建议

- 先用 `./local_debug.sh sendfile 16` 验证本地脚本、编译链和基础网络路径
- 需要调试 RDMA 连接或完成队列时，再切到 `./local_debug.sh simulate 64`
- 如果 RDMA 环境响应较慢，可以设置 `RDMA_CM_TIMEOUT_MS` 或 `RDMA_WC_TIMEOUT_MS` 调整超时时间

示例：

```bash
RDMA_CM_TIMEOUT_MS=30000 RDMA_WC_TIMEOUT_MS=30000 ./simulate_rdma
```