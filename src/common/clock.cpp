#include "common/clock.h"
#include <ctime>

std::int64_t SystemClock::nowUtc() const {
    return static_cast<std::int64_t>(std::time(nullptr));
}

SystemClock& systemClock() {
    static SystemClock clock;
    return clock;
}
