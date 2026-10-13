// 统一错误定义：模块返回错误码，界面通过它显示错误名称和中文说明。
#include "common/error.h"

// 把枚举错误码转成接口使用的固定英文名称。
const char* errorName(ErrorCode code) {
    switch (code) {
    case ErrorCode::Ok: return "OK";
    case ErrorCode::BadRequest: return "BAD_REQUEST";
    case ErrorCode::DictNotReady: return "DICT_NOT_READY";
    case ErrorCode::WordNotFound: return "WORD_NOT_FOUND";
    case ErrorCode::QuestionExpired: return "QUESTION_EXPIRED";
    case ErrorCode::DbError: return "DB_ERROR";
    case ErrorCode::AiDisabled: return "AI_DISABLED";
    case ErrorCode::AiKeyMissing: return "AI_KEY_MISSING";
    case ErrorCode::AiTimeout: return "AI_TIMEOUT";
    case ErrorCode::AiInvalid: return "AI_INVALID";
    case ErrorCode::Forbidden: return "FORBIDDEN";
    case ErrorCode::Internal: return "INTERNAL";
    }
    return "INTERNAL";
}

// 把枚举错误码转成用户可以读懂的中文说明。
const char* errorMessage(ErrorCode code) {
    switch (code) {
    case ErrorCode::Ok: return "";
    case ErrorCode::BadRequest: return "参数错误";
    case ErrorCode::DictNotReady: return "尚未导入词库";
    case ErrorCode::WordNotFound: return "未找到单词";
    case ErrorCode::QuestionExpired: return "题目已失效";
    case ErrorCode::DbError: return "数据库错误";
    case ErrorCode::AiDisabled: return "AI 未开启";
    case ErrorCode::AiKeyMissing: return "未配置密钥";
    case ErrorCode::AiTimeout: return "AI 请求超时";
    case ErrorCode::AiInvalid: return "AI 响应不合格";
    case ErrorCode::Forbidden: return "来源不被允许";
    case ErrorCode::Internal: return "内部错误";
    }
    return "内部错误";
}
