#include "processor/operator/multiplicity_reducer.h"

namespace kuzu {
namespace processor {

bool MultiplicityReducer::getNextTuplesInternal() {
    if (numRepeat == 0) {
        restoreMultiplicity();
        if (!children[0]->getNextTuple()) {
            return false;
        }
        saveMultiplicity();
        resultSet->multiplicity = 1;
    }
    numRepeat++;
    if (numRepeat == prevMultiplicity) {
        numRepeat = 0;
    }
    return true;
}

} // namespace processor
} // namespace kuzu
