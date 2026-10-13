// 统一返回值 Result<T>：code 表示成功或失败，成功时从 value 取数据。
#pragma once
#include "common/error.h"

template <typename T>
// 带错误码的返回值：调用方先看 ok()，成功后再读 value。
struct Result {
    // code=Ok 才表示成功；value 的实际类型由 T 决定，例如 Entry 或 Stats。
    ErrorCode code;
    T value;

    Result() : code(ErrorCode::Ok), value() {}

    // 检查本次调用是否成功；成功后再使用 value。
    bool ok() const { return code == ErrorCode::Ok; }

    // 构造成功结果，把业务数据放进 value。
    static Result success(const T& v) {
        Result r;
        r.code = ErrorCode::Ok;
        r.value = v;
        return r;
    }

    // 构造失败结果，只设置错误码；调用者不应把 value 当作有效结果。
    static Result fail(ErrorCode c) {
        Result r;
        r.code = c;
        return r;
    }
};
