#pragma once
#include "common/types.h"

class StageSource {
public:
    virtual ~StageSource() = default;
    virtual int stageOf(EntryId id) const = 0;
};
