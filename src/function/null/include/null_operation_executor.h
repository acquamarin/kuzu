#pragma once

#include "src/function/include/unary_operation_executor.h"

namespace graphflow {
namespace function {

struct NullOperationExecutor {

    template<typename FUNC>
    static void execute(ValueVector& operand, ValueVector& result) {
        assert(result.dataType.typeID == BOOL);
        result.state = operand.state;
        auto operandValues = (uint8_t*)operand.values;
        auto resultValues = (uint8_t*)result.values;
        if (operand.state->isFlat()) {
            auto pos = operand.state->getPositionOfCurrIdx();
            FUNC::operation(operandValues[pos], (bool)operand.isNull(pos), resultValues[pos]);
        } else {
            if (operand.state->isUnfiltered()) {
                for (auto i = 0u; i < operand.state->selectedSize; i++) {
                    FUNC::operation(operandValues[i], (bool)operand.isNull(i), resultValues[i]);
                }
            } else {
                for (auto i = 0u; i < operand.state->selectedSize; i++) {
                    auto pos = operand.state->selectedPositions[i];
                    FUNC::operation(
                        operandValues[pos], (bool)operand.isNull(pos), resultValues[pos]);
                }
            }
        }
    }

    template<typename FUNC>
    static uint64_t select(ValueVector& operand, sel_t* selectedPositions) {
        if (operand.state->isFlat()) {
            auto pos = operand.state->getPositionOfCurrIdx();
            uint8_t resultValue = 0;
            FUNC::operation(operand.values[pos], operand.isNull(pos), resultValue);
            return resultValue == true;
        } else {
            uint64_t numSelectedValues = 0;
            for (auto i = 0ul; i < operand.state->selectedSize; i++) {
                auto pos = operand.state->selectedPositions[i];
                UnaryOperationExecutor::selectOnValue<
                    uint8_t /* operand type does not matter for null operations */, FUNC>(
                    operand, pos, numSelectedValues, selectedPositions);
            }
            return numSelectedValues;
        }
    }
};

} // namespace function
} // namespace graphflow
