#!/bin/bash

echo "========================================="
echo "Compiling RDMA vs Sendfile Test Suite"
echo "========================================="

# 清理旧文件
rm -f simulate_rdma test_sendfile compare_transfer

# 编译 simulate_rdma
echo "Compiling simulate_rdma.c..."
gcc -o simulate_rdma simulate_rdma.c rdma_transfer.c -O3 -lrdmacm -libverbs -lpthread
if [ $? -eq 0 ]; then
    echo "✓ simulate_rdma compiled successfully"
else
    echo "✗ simulate_rdma compilation failed"
    exit 1
fi

# 编译 test_sendfile
echo "Compiling test_sendfile.c..."
gcc -o test_sendfile test_sendfile.c -O3 -lpthread
if [ $? -eq 0 ]; then
    echo "✓ test_sendfile compiled successfully"
else
    echo "✗ test_sendfile compilation failed"
    exit 1
fi

# 编译 compare_transfer
echo "Compiling compare_transfer.c..."
gcc -o compare_transfer compare_transfer.c rdma_transfer.c -O3 -lrdmacm -libverbs -lpthread
if [ $? -eq 0 ]; then
    echo "✓ compare_transfer compiled successfully"
else
    echo "✗ compare_transfer compilation failed"
    exit 1
fi

echo ""
echo "========================================="
echo "All programs compiled successfully!"
echo "========================================="
echo ""
echo "To run tests:"
echo "  ./simulate_rdma      # Test RDMA simulation"
echo "  ./test_sendfile      # Test sendfile"
echo "  ./local_debug.sh sendfile 64   # Local debug entry with a smaller file"
echo "  ./compare_transfer receive <bind_ip>   # Run on receiver machine"
echo "  ./compare_transfer send <receiver_ip>  # Run on sender machine"