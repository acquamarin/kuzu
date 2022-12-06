#pragma once

#include "processor/operator/physical_operator.h"
#include "storage/storage_structure/lists/lists.h"

namespace kuzu {
namespace processor {

class ScanList : public PhysicalOperator {

public:
    ScanList(const DataPos& inDataPos, const DataPos& outDataPos, Lists* lists,
        unique_ptr<PhysicalOperator> child, uint32_t id, const string& paramsString)
        : PhysicalOperator{move(child), id, paramsString}, inDataPos{inDataPos},
          outDataPos{outDataPos}, lists{lists} {}

    ~ScanList() override{};

    PhysicalOperatorType getOperatorType() override = 0;

    inline void initLocalStateInternal(ResultSet* resultSet, ExecutionContext* context) override {
        inValueVector = resultSet->getValueVector(inDataPos);
        outValueVector = resultSet->getValueVector(outDataPos);
    }

protected:
    DataPos inDataPos;
    DataPos outDataPos;

    shared_ptr<ValueVector> inValueVector;
    shared_ptr<ValueVector> outValueVector;

    Lists* lists;
    shared_ptr<ListHandle> listHandle;
};

} // namespace processor
} // namespace kuzu
