#pragma once
#include <cstdint>

class Clock {
public:
    virtual ~Clock() = default;
    virtual std::int64_t nowUtc() const = 0;
};

class SystemClock : public Clock {
public:
    std::int64_t nowUtc() const override;
};

SystemClock& systemClock();
