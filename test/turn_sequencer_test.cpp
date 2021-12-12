#include <iostream>
#include <thread>

#include "detail/turn_sequencer.h"
#include "gtest/gtest.h"

using namespace myfolly;
using namespace myfolly::detail;

TEST(TurnSequencerTest, mt_sequence) {
    TurnSequencer seq;
    std::atomic<uint32_t> spinCutoff{0};
    std::vector<std::unique_ptr<std::thread>> threads{10};
    for(int i=9;i>=0;--i) {
        threads[i].reset(new std::thread([&seq, &spinCutoff, i]() {
                seq.waitForTurn(i, spinCutoff, (i % 32) == 0);
                std::cout << "thread " << i << std::endl;
                seq.completeTurn(i);
                }));
    }

    for(int i=0;i<10;++i) {
        if(threads[i]->joinable()) {
            threads[i]->join();
        }
    }
    threads.clear();
}

int main(int argc, char* argv[]) {
    testing::InitGoogleTest(&argc, argv);

    return RUN_ALL_TESTS();
}
