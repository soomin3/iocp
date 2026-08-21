#pragma once
#include <cstdint>

struct SLoginProcedureResult
{
    bool Executed = false;
    int32_t ResultCode = -1;
    int64_t AccountID = 0;
};