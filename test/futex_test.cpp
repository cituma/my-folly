#include <iostream>
#include <thread>

#include "detail/futex.h"
#include "gtest/gtest.h"

using namespace myfolly;

class FutexDemo {
public:
    FutexDemo();
    ~FutexDemo();

    void Wait(uint32_t wait_mask);
    void Wake(uint32_t wait_mask);
private:
    std::atomic<uint32_t> _state;
};

FutexDemo::FutexDemo() :
    _state(0) {
}

FutexDemo::~FutexDemo() {
}

void FutexDemo::Wait(uint32_t wait_mask) {
    uint32_t state = _state.load(std::memory_order_acquire);
    detail::futexWait(&_state, state, wait_mask);
}

void FutexDemo::Wake(uint32_t wait_mask) {
    detail::futexWake(&_state, std::numeric_limits<int>::max(), wait_mask);
}

TEST(FutexTest, spsc) {
    FutexDemo futex_demo;

    std::thread t1([&futex_demo]() {
            futex_demo.Wait(0xFFFFFFFF);
            std::cout << "thread1" << std::endl;
            });

    std::this_thread::sleep_for(std::chrono::microseconds(100000));     //sleep 100ms
    std::thread t2([&futex_demo]() {
            std::cout << "thread2" << std::endl;
            futex_demo.Wake(0xFFFFFFFF);
            });

    if(t1.joinable()) {
        t1.join();
    }
    if(t2.joinable()) {
        t2.join();
    }
}

int main(int argc, char* argv[]) {
    testing::InitGoogleTest(&argc, argv);

    return RUN_ALL_TESTS();
}
