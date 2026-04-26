
template <typename T>;
class BlockingQueue {
public:
    BlockingQueue(bool nonblock = false) : nonblock_(nonblock) {}    
    void Push(const T& value) {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(value);
        cond_.notify_one();
    }
    bool Pop(&T value) {
        std::unique_lock<std::mutex> lock(mutex_);
        cond_.wait(lock, [this]{ !cond_.empty() || nonblock_ } );
        if (queue_.empty()) {
            return false;
        }
        return true;
    }
    void Destroy() {
        std::lock_guard<std::mutex> lock(mutex_);
        nonblock_ = true;
        cond_.notify_all();
    }
private:
    bool nonblock_;
    std::queue<T> queue_;
    std::condition_variable cond_;
    std::mutex mutex_;    
};

class ThreadPool {
public:
    ThreadPool(int worker_num)  {
        for (int i = 0; i < worker_num; ++i) {
            workers_.emplace_back([this]{Worker();});
        }
    }
    ~ThreadPool() {
        task_queue_.Destroy();
        for (auto &work_thread : workers_) {
            if (work_thread.joinable()) {
                work_thread.join();
            }
        }
    }    
    void Post(std::function<void()> task) {
        task_queue_.Push(task);
    }
private:
    void Worker() {
        std::function<void()> task;
        while (true) {
            if (!task_queue_.Pop(task)) {
                break;
            } 
            task();                     
        }        
    }
    BlockingQueue<std::function<void()>> task_queue_;
    vector<std::thread> workers_;    
};