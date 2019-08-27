// (C)opyright 2019 awemany, see file COPYING for details
#include <string>
#include <random>
#include "tinyformat.h"
#include "deltablocks.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        tfm::printf("Need maximum time to simulate to to do as command line argument.\n");
        return 1;
    }
    uint64_t seed = 0;
    if (argc > 2) {
        seed = std::stoull(argv[2]);
    } else {
        std::random_device randdev;
        seed = randdev();
        seed |= (uint64_t)randdev()<<32;
    }
    tfm::printf("RNG SEED VALUE %d\n", seed);
    auto scheduler = deltablocksSetup(seed);
    uint64_t max_time = std::stol(argv[1]);
    scheduler->run(max_time);
    printTxnFirstConfirmStats();
    return 0;
}
