// 统一时间来源：业务使用 UTC 秒；测试可以提供自己的时钟。
#include "common/clock.h"
#include <ctime>

// 返回 UTC 秒时间，用于保存事件时间或计算复习到期时间。
std::int64_t SystemClock::nowUtc() const {
    return static_cast<std::int64_t>(std::time(nullptr));
}

// 返回共用的系统时钟对象，避免各模块各自创建。
SystemClock& systemClock() {
    static SystemClock clock;
    return clock;
}
