#pragma once//?这是什么
//
#include <functional>
#include <queue>
#include <mutex>
#include <thread>

template <typename T>
class BlockingQueue {
public: 

    BlockingQueue(bool nonblock = false) : nonblock_(nonblock) {
        
    }
    void Push(const T &value) {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(value);
        not_empty_.notify_one();
    }

    //正常 pop 弹出元素
    //异常 pop 没有返回元素
    //为空的时候阻塞队列
    bool Pop(T &value) {
        //可手动unlock
        std::unique_lock<std::mutex> lock(mutex_);

        // 1. mutex_.unlock()
        // 2. 条件queue_.empty() && !nonblock_ 线程在wait中阻塞
        // notify_one notify_all唤醒线程
        // 3. 假设满足条件 mutex_.lock()
        // 4. 不满足条件又回到2
        not_empty_.wait(lock, [this]{ return !queue_.empty() || nonblock_;});
        if (queue_.empty()) return false;

        value = queue_.front();
        queue_.pop();
        return true;
    }

    //解除阻塞
    void Cancel() {
        std::lock_guard<std::mutex> lock(mutex_);
        nonblock_ = true;
        not_empty_.notify_all;
    }

private:
    bool nonblock_;
    std::queue<T> queue_;
    std::mutex mutex_;
    std::condition_variable not_empty_; 
    
};
class ThreadPool {
public:
    //线程池初始化
    explicit ThreadPool(int thread_num) {
        for (size_t i = 0; i < threads_num; ++i) {
            workers_.emplace_back([this]{Workers();});
        }
    }

    ~ThreadPool() {
        //析构函数
        task_queue_.Cancel();
        for (auto &workers: workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }
    void Post(std::function<void()> task) {//异步
        task_queue_.push(task);
    }

private:
    void Workers() {
        while (true) {
            std::function<void()> task;
            if (!task_queue_.Pop(task)) {
                break;
            }
            task();
        }
    }
    //std的queue不是线程安全的,以下代码不好
    //std::queue<std::function<void()>> queue_;    
    //用阻塞队列
    BlockingQueue<std::function<void()>> task_queue_;
    //线程集合
    std::vector<std::thread> workers_;
};
