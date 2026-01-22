#pragma once

#include "common/tuple.h"

namespace sql {

class Operator {
public:
    virtual ~Operator() = default;
    
    virtual void Open() = 0;
    virtual bool Next(Tuple* tuple) = 0;
    virtual void Close() = 0;
};

} // namespace sql
