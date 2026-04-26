class mmpool {
    explicit mmpool(std::size_t initial_blocks, std::size_t block_size = 0) {
        for (int i = 0; i < initial_blocks; ++i) {
            blocks_.push_back(Block{std::vector<std::byte>(block_size), false});
        }
    }

    std::byte* allocate(std::size_t size) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto &block : blocks_) {
            if (!block.in_use && block.size() >= size) {
                block.in_use = true;
                return block.buffer.data();
            }
        }
        blocks_.push_back(Block{std::vector<std::byte> (size), true});
        return blocks_.back().buffer.data();
    }

    void deallocate(std::byte* ptr) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto &block : blocks_) {
            if (block.buffer.data() == ptr) {
                block.in_use = false;
                return;
            }
        }
    }

    int total_blocks() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return blocks_.size();
    }

    int used_blocks() const {
        std::lock_guard<std::mutex> lock(mutex_);
        int cnt = 0;
        for (auto& block : blocks_) {
            if (block.in_use) {
                ++cnt;
            }
        }
        return cnt;
    }

private:
    struct Block {
        std::vector<std::byte> buffer;
        bool in_use = false;
    };
    mutable std::mutex mutex_;
    std::vector<Block> blocks_;
};

