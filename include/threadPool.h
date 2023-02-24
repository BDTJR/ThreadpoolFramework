#pragma once
#include <iostream>
#include <queue>
#include <stack>
#include <algorithm>
#include <future>
#include <unordered_map>
#include <vector>
#include <thread>
#include <functional>
#include <condition_variable>
#include <atomic>

namespace threadPool {


/**
 * 封装任务类型
 * 
 */
class Task {
    //为了把任务函数的返回类型抽象成Func_T，而不是void
    struct BaseExecutableTask {
        virtual ~BaseExecutableTask() {};
        virtual void run() = 0;
    };

    template <typename Func_T>   
    struct ExecutableTask: BaseExecutableTask {
        Func_T func;
        ExecutableTask(Func_T&& f):func(std::forward<Func_T>(f)){}; //必须要用带有初始化列表的构造函数，这是因为模板成员是根据实际的模板参数进行实例化的
        void run() override {func();};
    };
public: 
    Task () = default;
    Task(Task&) = delete;
    Task(const Task&) = delete;
    Task& operator = (const Task&) = delete;
    Task& operator = (Task&& t) {
            delete p;
            p = t.p;
            t.p = nullptr;  
            return *this;
    }
    template<typename Func_T>
    Task(Func_T&& func) {
        p = new ExecutableTask<Func_T>(std::forward<Func_T>(func));
    }
    ~Task() {delete p;}
    void operator()() {
        p->run();
    }
private:
    BaseExecutableTask* p = nullptr; //指向基类指针，可以不明确给出typename Func_T
};

/**
 * 每个线程都有一个线程安全的双端任务队列
 * 队首支持本地线程取任务，对尾支持其他线程偷任务
 * 
 */
template<typename T>
class ThreadSafeDeque {
public:
    ThreadSafeDeque() = default;

    void push_front(T&& value) {
        std::lock_guard<std::mutex> lock(mutex_);
        deque_.emplace_front(std::move(value));
        cv_.notify_one();
    }

    void push_back(T&& value) {
        std::lock_guard<std::mutex> lock(mutex_);
        deque_.emplace_back(std::move(value));
        cv_.notify_one();
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return deque_.empty();
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return deque_.size();
    }

    bool try_pop_front(T& value) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (deque_.empty()) {
            return false;
        }
        value = std::move(deque_.front());
        deque_.pop_front();
        return true;
    }

    void wait_pop_front(T& value) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return !deque_.empty(); });
        value = std::move(deque_.front());
        deque_.pop_front();
    }

    bool try_pop_back(T& value) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (deque_.empty()) {
            return false;
        }
        value = std::move(deque_.back());
        deque_.pop_back();
        return true;
    }

    void wait_pop_back(T& value) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return !deque_.empty(); });
        value = std::move(deque_.back());
        deque_.pop_back();
    }

private:
    std::deque<T> deque_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
};

class ThreadPool
{
public:
    ThreadPool(size_t threadNum) {
        for (size_t i = 0; i < threadNum; ++i) {
            mThreadsTasks.emplace_back(new ThreadTasks());
        }
        for (size_t i = 0; i < threadNum; ++i) {           
            mThreads.emplace_back([this, i] {
                localWorkQueue = mThreadsTasks[i].get();
                Task task;
                while (true) {
                    if (!mStopping && tryGetTask(task, i)) {
                        task();
                        continue;
                    }
                
                    if (mStopping && commTasks.empty()) {
                        break;
                    }

                    {
                        std::unique_lock<std::mutex> lock{ mTaskMutex };
                        mCondition.wait(lock);
                    } 
                    
                    /* {
                        std::unique_lock<std::mutex> lock{ mTaskMutex };
                        mCondition.wait(lock, [this] { return mStopping || !commTasks.empty(); });

                        if (mStopping && commTasks.empty()) {
                            return;
                        }

                        task = std::move(commTasks.front());
                        commTasks.pop();
                    }
                    task(); */
                }
            });
            ThreadToTaskQueue[mThreads.back().get_id()] = i;
        }
        
    }

    ~ThreadPool() {
        {
            std::unique_lock<std::mutex> lock{ mEventMutex };
            mStopping = true;
        }
        mCondition.notify_all();

        for (auto &thread : mThreads) {
            thread.join();
        }
    }
    /**
     * @brief 提交任务
     * 
     * @tparam Func_T 
     * @param f 
     */
    template <typename Func_T>
    void execute(Func_T&& f) {
        if (localWorkQueue) {
            localWorkQueue->push_back(std::forward<Func_T>(f));
        }
        else  {
            std::unique_lock<std::mutex> lock{ mTaskMutex };
            commTasks.emplace(std::forward<Func_T>(f));
        }
        mCondition.notify_one();
    }
    /**
     * @brief 提交任务并返回结果
     * 
     * @tparam Func_T 
     * @param f 
     * @return std::future<typename std::result_of<Func_T()>::type> 
     */
    template <typename Func_T>
    auto submit(Func_T&& f) -> std::future<typename std::result_of<Func_T()>::type> {
        using return_type = typename std::result_of<Func_T()>::type;
        std::packaged_task<return_type()> pack(std::move(f));
        std::future<return_type> result(pack.get_future());
        if (localWorkQueue) {
                localWorkQueue->push_back(std::forward<Func_T>(f));
        }
        else  {
            std::unique_lock<std::mutex> lock{ mTaskMutex };
            if (mStopping) {
                throw std::runtime_error("enqueue on stopped ThreadPool");
            }
            commTasks.emplace(std::move(pack));
        }
        mCondition.notify_one();
        return result;
    }

private:
    std::vector<std::thread> mThreads;
    std::queue<Task> commTasks;

    typedef ThreadSafeDeque<Task> ThreadTasks;
    typedef std::shared_ptr<ThreadTasks> TaskQueuePtr;
    std::unordered_map<std::thread::id, size_t> ThreadToTaskQueue;
    std::vector<TaskQueuePtr> mThreadsTasks;
    static thread_local ThreadTasks *localWorkQueue;

    std::mutex mTaskMutex;
    std::mutex mEventMutex;
    std::condition_variable mCondition;

    bool mStopping = false;

    bool tryGetTask(Task &task, size_t idx) {
        auto& localDeque = mThreadsTasks[idx];
         // try local deque first
        if (localDeque->try_pop_front(task)) {
            return true;
        }   
        // try steal others
        if (tryStealOne(task, idx)) {
            return true;
        }
        // try common queue
        std::unique_lock<std::mutex> lock{ mTaskMutex };
        if (!commTasks.empty()) {
            task = std::move(commTasks.front());
            commTasks.pop();
            return true;
        }
        lock.unlock();
        return false;
    }

    bool tryStealOne(Task &task, size_t skipIdx) {
        size_t offset = std::rand() % mThreadsTasks.size();
        for (size_t i = 0; i < mThreadsTasks.size(); ++i) {
            size_t idx = (i + offset) % mThreadsTasks.size();
            if (idx == skipIdx) {
                continue;
            }
            auto& anotherDeque = mThreadsTasks[idx];
            if (anotherDeque->try_pop_front(task)) {
                if (anotherDeque->size() > 0) { //窃取的队列如果还有任务, 则应该notify其他可能正在阻塞的工作线程
                    mCondition.notify_one();
                }
                return true;
            }
        }
        return false;
    }
};
}
