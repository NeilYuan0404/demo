//RAII 
class HandleMemoryPool {
public:
    //只读时候快照的块
    struct BlockIndo {
        std::size_t index;
        std::size_t capacity;
        bool in_use;
    };

    //Handle类似于unique_ptr
    class Handle {
    public:
        //允许空构造
        Handle() = default;
        Handle(HandleMemoryPool* pool, std::size_t index, std::byte* data, std::size_t size)
            : pool_(pool), index_(index), data_(data), size_(size), owns_(true) {}
        // = delete ：禁用以下函数
        Handle(const Handle&) = delete;
        Handle& operator=(const Handle&) = delete;

        Handle(Handle& other) noexcept {move_from(std::move(other));}

        //右值引用，传进move中，偷other的东西
        Handle& operator=(Handle&& other) noexcept {
            if (this != &other) {
                reset();
                move_from(std::move(other));
            }
            return *this;
        }

        ~Handle() {reset();}

        //通过Handle来访问内部的资料
        std::byte* data() {return data_;}
        const std::byte* data() const {return data_;}
        std::size_t size() const {return size_;}
        //也就是说，对于一个对象而言，你的成员函数名字叫operator bool，
        //如果重载了此函数，那么发生隐式转换时，
        //编译器就自动参照这个名字所代表的函数进行操作
        //作用：让对象可以像布尔值一样使用 如：if(Handle实例)
        explicit operator bool() const {return owns_;} 
    
    private:
        //把Handle移动到内部函数据，实现移动语义
        void move_from(Handle&& other) noexcept {
            pool_ = other.pool_;
            size_ = other.size_;
            data_ = other.data_;
            index_ = other.index_;
            owns_ = other.owns_;

            other.pool_ = nullptr;
            other.size_ = 0;
            other.data_ = nullptr;
            other.index_ = 0;
            other.owns_ = false;
        }

        void reset() {
            if (owns_ && pool_ != nullptr) {
                pool_->release(index_);
                pool_ = nullptr;
                data_ = nullptr;
                size_ = 0;
                owns_ = false;
            }
        }

        HandleMemoryPool* pool_ = nullptr; //可以用weakptr, 外面用shared_ptr
        std::size_t size_ = 0;
        std::byte* data_ = nullptr;
        std::size_t index_ = 0;
        bool owns_ = false;
    };

    Handle allocate(std::size_t size) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        //优先考虑复用
        for (std::size_t i = 0; i < blocks_.size(); ++i) {
            auto& block = blocks_[i];
            if (!block.empty() && block.buffer.size() >= size) {
                block.in_use = true;
                return Handle(this, i, block.buffer.data(), size);
            }
        }

        blocks_.push_back(Block{std::vector<std::byte>(size), true});
        auto& block = blocks_.back();
        return Handle(this, blocks_.size()-1, block_buffer.data(), size);
    }

    void deallocate() {

    }

    std::size_t total_block() const {
        std::lock_guard<std::shared_mutex> lock(mutex_);
        return blocks_.size();        
    }

    int used_block() const {
        std::lock_guard<std::shared_mutex> lock(mutex_);
        std::size_t count = 0;
        for (blocks_.in_use) {
            ++count;
        }
        return count;
    }

private:
    struct Block {
        std::vector<std::byte> buffer;
        bool in_use = false;
    };

    void release(std::size_t index) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        if (index < blocks_.size()) {
            blocks_[index].in_use = false;
        }
    }

    std::vector<Block> blocks_;
    mutable std::shared_mutex mutex_;
};