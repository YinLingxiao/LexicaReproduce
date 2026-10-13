// 统一错误定义：模块返回错误码，界面通过它显示错误名称和中文说明。
#pragma once

// 所有业务模块共用的错误码，Ok 表示成功。
enum class ErrorCode {
    Ok = 0,
    BadRequest,
    DictNotReady,
    WordNotFound,
    QuestionExpired,
    DbError,
    AiDisabled,
    AiKeyMissing,
    AiTimeout,
    AiInvalid,
    Forbidden,
    Internal
};

// 把枚举错误码转成接口使用的固定英文名称。
const char* errorName(ErrorCode code);
// 把枚举错误码转成用户可以读懂的中文说明。
const char* errorMessage(ErrorCode code);
