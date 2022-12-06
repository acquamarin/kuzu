#include "processor/operator/flatten.h"

namespace kuzu {
namespace processor {

void Flatten::initLocalStateInternal(ResultSet* resultSet, ExecutionContext* context) {
    dataChunkToFlatten = resultSet->dataChunks[dataChunkToFlattenPos];
}

bool Flatten::getNextTuplesInternal() {
    // currentIdx == -1 is the check for initial case
    if (dataChunkToFlatten->state->currIdx == -1 || dataChunkToFlatten->state->isCurrIdxLast()) {
        dataChunkToFlatten->state->currIdx = -1;
        if (!children[0]->getNextTuple()) {
            return false;
        }
    }
    dataChunkToFlatten->state->currIdx++;
    metrics->numOutputTuple.incrementByOne();
    return true;
}

} // namespace processor
} // namespace kuzu
