#pragma once

#include "execution/operator.h"

namespace sql {

class Projection : public Operator {
public:
    void Open() override;
    bool Next(Tuple* tuple) override;
    void Close() override;
};

} // namespace sql
