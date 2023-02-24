#include "../include/threadPool.h"
using namespace threadPool;

typedef ThreadSafeDeque<Task> ThreadTasks;
thread_local ThreadTasks* ThreadPool::localWorkQueue = nullptr;


int main()
{ 
    ThreadPool pool(8);
    int taskNum = 10000;
    std::atomic_int var = {0};
    for (int i = 0; i < taskNum; ++i) { 
        pool.execute([&]{ var++; });
    }
     for (int i = 0; i < taskNum; ++i) { 
        pool.submit([&]{ var++; });
    }
    std::this_thread::sleep_for(std::chrono::seconds(1));
    std::cout << var.load() << std::endl;
}