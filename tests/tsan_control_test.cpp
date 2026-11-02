#include <cstdlib>
#include <iostream>
#include <thread>

// A deliberate data race, built only under ThreadSanitizer, where CTest expects this program to
// fail. So a passing ThreadSanitizer job means something: if the sanitizer stopped seeing races,
// this test would start to pass as a program, and fail as a test.
namespace {

// volatile, so the optimizer keeps every access, and not atomic, so they race.
volatile int shared = 0;

void bump() {
    for (int i = 0; i < 100000; ++i) {
        shared = shared + 1;
    }
}

}  // namespace

int main() {
    std::thread first(bump);
    std::thread second(bump);
    first.join();
    second.join();
    std::cout << "raced to " << shared << std::endl;
    return EXIT_SUCCESS;
}
