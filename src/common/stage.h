// 阶段查询接口：个人词汇模块只通过 stageOf 读取复习模块的学习阶段，不修改复习状态。
#pragma once
#include "common/types.h"

// 只读阶段接口，让个人词汇模块可以查询复习模块，而不用依赖复习模块的内部实现。
class StageSource {
public:
    virtual ~StageSource() = default;
    // 读取一个词当前的学习阶段；没有记录时返回 0。
    virtual int stageOf(EntryId id) const = 0;
};
