// 统一时间来源：业务使用 UTC 秒；测试可以提供自己的时钟。
#pragma once
#include <cstdint>

// 时间接口：真实时钟和测试时钟都提供 nowUtc()。
class Clock {
public:
    virtual ~Clock() = default;
    // 返回 UTC 秒时间，用于保存事件时间或计算复习到期时间。
    virtual std::int64_t nowUtc() const = 0;
};

// 真实系统时钟，返回自 1970 年起的 UTC 秒数。
class SystemClock : public Clock {
public:
    // 返回 UTC 秒时间，用于保存事件时间或计算复习到期时间。
    std::int64_t nowUtc() const override;
};

// 返回共用的系统时钟对象，避免各模块各自创建。
SystemClock& systemClock();
