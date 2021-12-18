#include <iostream>
#include <functional>
#include <memory>
#include <string>
#include <chrono>
#include <thread>
#include <utility>
#include <queue>
#include <mutex>
#include <iomanip>

#include "mpmc_queue.h"
#include "bounded_queue.h"

using namespace myfolly;

static uint64_t now_real_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>
      (std::chrono::system_clock::now().time_since_epoch()).count();
}

template <typename T>
class NormalQueue {
public:
    NormalQueue(size_t cap) :
        _capacity(cap) {}

    bool write(T const& val) {
        std::lock_guard<std::mutex> lk(_mutex);
        if(_q.size() >= _capacity) {
            return false;
        }
        _q.push(val);
        return true;
    }

    void blockingWrite(T const& val) {
        while(true) {
            while(_q.size() >= _capacity) {
                std::this_thread::yield();
            }
            std::lock_guard<std::mutex> lk(_mutex);
            if(_q.size() >= _capacity) {
                continue;
            }
            _q.push(val);
            break;
        }
    }

    bool read(T& val) {
        std::lock_guard<std::mutex> lk(_mutex);
        if(_q.empty()) {
            return false;
        }
        val = _q.front();
        _q.pop();
        return true;
    }

    void blockingRead(T& val) {
        while(true) {
            while(_q.empty()) {
                std::this_thread::yield();
            }
            std::lock_guard<std::mutex> lk(_mutex);
            if(_q.empty()) {
                continue;
            }
            val = _q.front();
            _q.pop();
            break;
        }
    }

    bool isEmpty() {
        std::lock_guard<std::mutex> lk(_mutex);
        return _q.empty();
    }

private:
    size_t _capacity;
    std::queue<T> _q;
    std::mutex _mutex;
};

template <typename Q>
void runEnqThread(
        int numThreads,
        uint64_t n, /*numOps*/
        Q& cq,
        int t) {
    uint64_t src = t;
    while (src < n) {
        cq.blockingWrite(src);
        src += numThreads;
    }
}

template <typename Q>
void runDeqThread(
        int numThreads,
        uint64_t n, /*numOps*/
        Q& cq,
        std::atomic<uint64_t>& sum,
        int t) {
    uint64_t threadSum = 0;
    uint64_t received = t;
    while (received < n) {
        uint64_t dst = 0;
        cq.blockingRead(dst);
        received += numThreads;
        threadSum += dst;
    }
    sum += threadSum;
}

template <typename Q>
void runTryEnqDeqTest(int numThreads, int numOps) {
    Q cq(128);

    uint64_t n = numOps;
    std::vector<std::unique_ptr<std::thread>> push_threads(numThreads);
    std::vector<std::unique_ptr<std::thread>> pop_threads(numThreads);
    std::atomic<uint64_t> sum(0);
    for (int t = 0; t < numThreads; ++t) {
        push_threads[t].reset(new std::thread(std::bind(
                runEnqThread<Q>,
                numThreads,
                n,
                std::ref(cq),
                t)));
        pop_threads[t].reset(new std::thread(std::bind(
                runDeqThread<Q>,
                numThreads,
                n,
                std::ref(cq),
                std::ref(sum),
                t)));
    }
    for(auto& t : push_threads) {
        if(t->joinable()) {
            t->join();
        }
    }
    push_threads.clear();
    for(auto& t : pop_threads) {
        if(t->joinable()) {
            t->join();
        }
    }
    pop_threads.clear();
    if(!cq.isEmpty()) {
        std::cout << "ERROR Result! cq is not empty." << std::endl;
        return;
    }
    if(n * (n - 1) / 2 != sum) {
        std::cout << "ERROR Result! sum:" << n * (n - 1) / 2 << " : " << sum << std::endl;
    }
}

void mt_test_enq_deq() {
    int nts[] = {1, 4, 10, 50, 100};

    int32_t n = 1000000;
    {
        std::cout << "Test normal queue:" << std::endl;
        uint64_t all_time = 0;
        for (int nt : nts) {
            auto start = now_real_us();
            runTryEnqDeqTest<NormalQueue<uint64_t>>(nt, n);
            auto normal_queue_time = now_real_us() - start;
            std::cout << "thread num:" << std::setw(4) << nt
              << ". normal  queue time: " << normal_queue_time << " us" << std::endl;
            all_time += normal_queue_time;
        }
        std::cout << "normal  queue time: " << all_time << " us" << std::endl;
    }
    std::cout << std::endl;

    {
        std::cout << "Test bounded queue:" << std::endl;
        uint64_t all_time = 0;
        for (int nt : nts) {
            auto start = now_real_us();
            runTryEnqDeqTest<BoundedQueue<uint64_t>>(nt, n);
            auto run_time = now_real_us() - start;
            std::cout << "thread num:" << std::setw(4) << nt
              << ". bounded queue time: " << run_time << " us" << std::endl;
            all_time += run_time;
        }
        std::cout << "bounded queue time: " << all_time << " us" << std::endl;
    }
    std::cout << std::endl;

    {
        std::cout << "Test mpmc queue:" << std::endl;
        uint64_t all_time = 0;
        for (int nt : nts) {
            auto start = now_real_us();
            runTryEnqDeqTest<MPMCQueue<uint64_t>>(nt, n);
            auto mpmc_queue_time = now_real_us() - start;
            std::cout << "thread num:" << std::setw(4) << nt
              << ". mpmc    queue time: " << mpmc_queue_time << " us" << std::endl;
            all_time += mpmc_queue_time;
        }
        std::cout << "mpmc    queue time: " << all_time << " us" << std::endl;
    }
}

int main(int argc, char* argv[]) {
    std::cout << "Start MPMCQueueBenchmark!" << std::endl;
    mt_test_enq_deq();
    return 0;
}

