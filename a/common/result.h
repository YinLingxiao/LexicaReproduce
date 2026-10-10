#pragma once
#include "common/error.h"

template <typename T>
struct Result {
    ErrorCode code;
    T value;

    Result() : code(ErrorCode::Ok), value() {}

    bool ok() const { return code == ErrorCode::Ok; }

    static Result success(const T& v) {
        Result r;
        r.code = ErrorCode::Ok;
        r.value = v;
        return r;
    }

    static Result fail(ErrorCode c) {
        Result r;
        r.code = c;
        return r;
    }
};
