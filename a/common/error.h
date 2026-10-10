#pragma once

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

const char* errorName(ErrorCode code);
const char* errorMessage(ErrorCode code);
