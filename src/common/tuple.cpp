#include "common/tuple.h"
#include <sstream>

namespace sql
{

    std::string Tuple::ToString() const
    {
        std::stringstream ss;
        ss << "(";
        for (size_t i = 0; i < values_.size(); ++i)
        {
            if (i > 0)
            {
                ss << ", ";
            }
            ss << values_[i].ToString();
        }
        ss << ")";
        return ss.str();
    }

} // namespace sql
