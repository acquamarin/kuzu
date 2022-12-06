#include "processor/result/result_set_descriptor.h"

namespace kuzu {
namespace processor {

ResultSetDescriptor::ResultSetDescriptor(const Schema& schema) {
    for (auto i = 0u; i < schema.getNumGroups(); ++i) {
        auto group = schema.getGroup(i);
        auto dataChunkDescriptor = make_unique<DataChunkDescriptor>();
        if (group->isSingleState()) {
            dataChunkDescriptor->setSingleState();
        }
        for (auto& expression : group->getExpressions()) {
            expressionNameToDataChunkPosMap.insert(
                {expression->getUniqueName(), dataChunkDescriptors.size()});
            dataChunkDescriptor->addExpression(expression);
        }
        dataChunkDescriptors.push_back(std::move(dataChunkDescriptor));
    }
}

ResultSetDescriptor::ResultSetDescriptor(const ResultSetDescriptor& other)
    : expressionNameToDataChunkPosMap{other.expressionNameToDataChunkPosMap} {
    for (auto& dataChunkDescriptor : other.dataChunkDescriptors) {
        dataChunkDescriptors.push_back(make_unique<DataChunkDescriptor>(*dataChunkDescriptor));
    }
}

DataPos ResultSetDescriptor::getDataPos(const string& name) const {
    assert(expressionNameToDataChunkPosMap.contains(name));
    auto dataChunkPos = expressionNameToDataChunkPosMap.at(name);
    auto valueVectorPos = dataChunkDescriptors[dataChunkPos]->getValueVectorPos(name);
    return DataPos{dataChunkPos, valueVectorPos};
}

} // namespace processor
} // namespace kuzu
