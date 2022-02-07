#include "src/planner/include/enumerator.h"

#include "src/planner/include/logical_plan/operator/exists/logical_exist.h"
#include "src/planner/include/logical_plan/operator/extend/logical_extend.h"
#include "src/planner/include/logical_plan/operator/filter/logical_filter.h"
#include "src/planner/include/logical_plan/operator/flatten/logical_flatten.h"
#include "src/planner/include/logical_plan/operator/nested_loop_join/logical_left_nested_loop_join.h"
#include "src/planner/include/logical_plan/operator/scan_property/logical_scan_node_property.h"
#include "src/planner/include/logical_plan/operator/scan_property/logical_scan_rel_property.h"

namespace graphflow {
namespace planner {

vector<unique_ptr<LogicalPlan>> Enumerator::getValidSubPlans(
    vector<unique_ptr<LogicalPlan>> plans) {
    vector<unique_ptr<LogicalPlan>> results;
    for (auto& plan : plans) {
        if (plan->containOperatorsNotAllowedInSubPlan()) {
            continue;
        }
        results.push_back(move(plan));
    }
    return results;
}

unique_ptr<LogicalPlan> Enumerator::getBestPlan(vector<unique_ptr<LogicalPlan>> plans) {
    auto bestPlan = move(plans[0]);
    for (auto i = 1u; i < plans.size(); ++i) {
        if (plans[i]->cost < bestPlan->cost) {
            bestPlan = move(plans[i]);
        }
    }
    return bestPlan;
}

vector<unique_ptr<LogicalPlan>> Enumerator::enumeratePlans(const BoundSingleQuery& singleQuery) {
    auto normalizedQuery = QueryNormalizer::normalizeQuery(singleQuery);
    auto plans = getInitialEmptyPlans();
    for (auto& queryPart : normalizedQuery->getQueryParts()) {
        plans = enumerateQueryPart(*queryPart, move(plans));
    }
    return plans;
}

vector<unique_ptr<LogicalPlan>> Enumerator::enumerateQueryPart(
    const NormalizedQueryPart& queryPart, vector<unique_ptr<LogicalPlan>> prevPlans) {
    vector<unique_ptr<LogicalPlan>> plans = move(prevPlans);
    for (auto i = 0u; i < queryPart.getNumQueryGraph(); ++i) {
        if (queryPart.isQueryGraphOptional(i)) {
            auto& queryGraph = *queryPart.getQueryGraph(i);
            auto queryGraphPredicate = queryPart.getQueryGraphPredicate(i);
            for (auto& plan : plans) {
                planOptionalMatch(queryGraph, queryGraphPredicate, *plan);
            }
            // Although optional match is planned as a subquery, we still need to merge query graph
            // as we merge sub query schema into outer query.
            joinOrderEnumerator.context->mergeQueryGraph(queryGraph);
        } else {
            plans = joinOrderEnumerator.enumerateJoinOrder(
                *queryPart.getQueryGraph(i), queryPart.getQueryGraphPredicate(i), move(plans));
        }
    }
    projectionEnumerator.enumerateProjectionBody(
        *queryPart.getProjectionBody(), plans, queryPart.isLastQueryPart());
    if (queryPart.hasProjectionBodyPredicate()) {
        for (auto& plan : plans) {
            appendFilter(queryPart.getProjectionBodyPredicate(), *plan);
        }
    }
    return plans;
}

void Enumerator::planOptionalMatch(const QueryGraph& queryGraph,
    const shared_ptr<Expression>& queryGraphPredicate, LogicalPlan& outerPlan) {
    vector<shared_ptr<Expression>> expressionsToScanFromOuter;
    if (queryGraphPredicate) {
        expressionsToScanFromOuter =
            getSubExpressionsInSchema(queryGraphPredicate, *outerPlan.schema);
    }
    for (auto& nodeIDExpression : queryGraph.getNodeIDExpressions()) {
        if (outerPlan.schema->expressionInScope(nodeIDExpression->getUniqueName())) {
            expressionsToScanFromOuter.push_back(nodeIDExpression);
        }
    }
    for (auto& expression : expressionsToScanFromOuter) {
        appendFlattenIfNecessary(
            outerPlan.schema->getGroupPos(expression->getUniqueName()), outerPlan);
    }
    auto prevContext = joinOrderEnumerator.enterSubquery(move(expressionsToScanFromOuter));
    auto bestPlan = getBestPlan(getValidSubPlans(joinOrderEnumerator.enumerateJoinOrder(
        queryGraph, queryGraphPredicate, getInitialEmptyPlans())));
    joinOrderEnumerator.exitSubquery(move(prevContext));
    // Schema merging logic for optional match:
    //    Consider example MATCH (a:Person) OPTIONAL MATCH (a)-[:studyAt]->(b:Organisation)
    //    where (a)-[:studyAt]->(b:Organisation) is a column extend. The inner resultSet
    //    contains 1 dataChunk DCInner with 2 vectors. In the outer query, we shallow copy the
    //    vector b from the inner query. However we put b's vector not to a's datachunk DCOuterA but
    //    to a different datachunk DCOuterB. This is because the b's vector contains DCInner's
    //    state, which might be different than DCOuterA. To make sure that when vector b is copied
    //    to the outer query, we can keep DCInner's state, we copy B to a new datachunk.
    auto subPlanSchema = bestPlan->schema->copy();
    assert(subPlanSchema->getNumGroups() > 0);
    auto firstInnerGroup = subPlanSchema->getGroup(0);
    auto firstInnerGroupMapToOuterPos = UINT32_MAX;
    // Merge first inner group into outer. This merging handles the logic above.
    for (auto& expressionName : firstInnerGroup->expressionNames) {
        if (outerPlan.schema->expressionInScope(expressionName)) {
            continue;
        }
        if (firstInnerGroupMapToOuterPos == UINT32_MAX) {
            firstInnerGroupMapToOuterPos = outerPlan.schema->createGroup();
        }
        outerPlan.schema->insertToGroupAndScope(expressionName, firstInnerGroupMapToOuterPos);
        outerPlan.schema->getGroup(firstInnerGroupMapToOuterPos)->isFlat = firstInnerGroup->isFlat;
    }
    // Merge rest inner groups into outer.
    for (auto i = 1u; i < subPlanSchema->getNumGroups(); ++i) {
        auto outerPos = outerPlan.schema->createGroup();
        auto group = subPlanSchema->getGroup(i);
        for (auto& expressionName : group->expressionNames) {
            outerPlan.schema->insertToGroupAndScope(expressionName, outerPos);
        }
        outerPlan.schema->getGroup(outerPos)->isFlat = group->isFlat;
    }
    auto logicalLeftNestedLoopJoin = make_shared<LogicalLeftNestedLoopJoin>(
        bestPlan->schema->copy(), outerPlan.lastOperator, bestPlan->lastOperator);
    outerPlan.appendOperator(move(logicalLeftNestedLoopJoin));
}

void Enumerator::planExistsSubquery(
    const shared_ptr<ExistentialSubqueryExpression>& subqueryExpression, LogicalPlan& outerPlan) {
    auto expressionsToScanFromOuter =
        getSubExpressionsInSchema(subqueryExpression, *outerPlan.schema);
    // We flatten all dependent groups for subquery and the result of subquery evaluation can be
    // appended to any flat dependent group.
    auto groupPosToWrite = UINT32_MAX;
    for (auto& expression : expressionsToScanFromOuter) {
        groupPosToWrite = outerPlan.schema->getGroupPos(expression->getUniqueName());
        appendFlattenIfNecessary(groupPosToWrite, outerPlan);
    }
    outerPlan.schema->insertToGroupAndScope(subqueryExpression->getUniqueName(), groupPosToWrite);
    auto& normalizedQuery = *subqueryExpression->getNormalizedSubquery();
    auto prevContext = joinOrderEnumerator.enterSubquery(move(expressionsToScanFromOuter));
    auto plans = enumerateQueryPart(*normalizedQuery.getQueryPart(0), getInitialEmptyPlans());
    joinOrderEnumerator.context->clearExpressionsToScanFromOuter();
    for (auto i = 1u; i < normalizedQuery.getQueryParts().size(); ++i) {
        plans = enumerateQueryPart(*normalizedQuery.getQueryPart(i), move(plans));
    }
    joinOrderEnumerator.exitSubquery(move(prevContext));
    auto bestPlan = getBestPlan(getValidSubPlans(move(plans)));
    auto logicalExists = make_shared<LogicalExists>(subqueryExpression, bestPlan->schema->copy(),
        outerPlan.lastOperator, bestPlan->lastOperator);
    outerPlan.appendOperator(logicalExists);
}

void Enumerator::appendFlattens(const unordered_set<uint32_t>& groupsPos, LogicalPlan& plan) {
    for (auto& groupPos : groupsPos) {
        appendFlattenIfNecessary(groupPos, plan);
    }
}

uint32_t Enumerator::appendFlattensButOne(
    const unordered_set<uint32_t>& groupsPos, LogicalPlan& plan) {
    if (groupsPos.empty()) {
        // an expression may not depend on any group. E.g. COUNT(*).
        return UINT32_MAX;
    }
    vector<uint32_t> unFlatGroupsPos;
    for (auto& groupPos : groupsPos) {
        if (!plan.schema->getGroup(groupPos)->isFlat) {
            unFlatGroupsPos.push_back(groupPos);
        }
    }
    if (unFlatGroupsPos.empty()) {
        return *groupsPos.begin();
    }
    for (auto i = 1u; i < unFlatGroupsPos.size(); ++i) {
        appendFlattenIfNecessary(unFlatGroupsPos[i], plan);
    }
    return unFlatGroupsPos[0];
}

void Enumerator::appendFlattenIfNecessary(uint32_t groupPos, LogicalPlan& plan) {
    auto& group = *plan.schema->groups[groupPos];
    if (group.isFlat) {
        return;
    }
    plan.schema->flattenGroup(groupPos);
    plan.cost += group.estimatedCardinality;
    auto flatten = make_shared<LogicalFlatten>(group.getAnyExpressionName(), plan.lastOperator);
    plan.appendOperator(move(flatten));
}

void Enumerator::appendFilter(const shared_ptr<Expression>& expression, LogicalPlan& plan) {
    appendScanPropertiesAndPlanSubqueryIfNecessary(expression, plan);
    auto dependentGroupsPos = getDependentGroupsPos(expression, *plan.schema);
    auto groupPosToSelect = appendFlattensButOne(dependentGroupsPos, plan);
    auto filter = make_shared<LogicalFilter>(expression, groupPosToSelect, plan.lastOperator);
    plan.schema->groups[groupPosToSelect]->estimatedCardinality *= PREDICATE_SELECTIVITY;
    plan.appendOperator(move(filter));
}

void Enumerator::appendScanPropertiesAndPlanSubqueryIfNecessary(
    const shared_ptr<Expression>& expression, LogicalPlan& plan) {
    appendScanPropertiesIfNecessary(expression, plan);
    if (expression->hasSubqueryExpression()) {
        auto expressions = expression->getTopLevelSubSubqueryExpressions();
        for (auto& expr : expressions) {
            auto subqueryExpression = static_pointer_cast<ExistentialSubqueryExpression>(expr);
            planExistsSubquery(subqueryExpression, plan);
        }
    }
}

void Enumerator::appendScanPropertiesIfNecessary(
    const shared_ptr<Expression>& expression, LogicalPlan& plan) {
    for (auto& expr : getPropertyExpressionsNotInSchema(expression, *plan.schema)) {
        auto& propertyExpression = (PropertyExpression&)*expr;
        // skip properties that has been evaluated
        if (plan.schema->expressionInScope(propertyExpression.getUniqueName())) {
            continue;
        }
        NODE == propertyExpression.getChild(0)->dataType ?
            appendScanNodePropertyIfNecessary(propertyExpression, plan) :
            appendScanRelPropertyIfNecessary(propertyExpression, plan);
    }
}

void Enumerator::appendScanNodePropertyIfNecessary(
    const PropertyExpression& propertyExpression, LogicalPlan& plan) {
    auto& nodeExpression = (const NodeExpression&)*propertyExpression.getChild(0);
    if (!plan.schema->expressionInScope(nodeExpression.getIDProperty())) {
        return;
    }
    auto scanProperty = make_shared<LogicalScanNodeProperty>(nodeExpression.getIDProperty(),
        nodeExpression.label, propertyExpression.getUniqueName(), propertyExpression.propertyKey,
        UNSTRUCTURED == propertyExpression.dataType, plan.lastOperator);
    auto groupPos = plan.schema->getGroupPos(nodeExpression.getIDProperty());
    plan.schema->insertToGroupAndScope(propertyExpression.getUniqueName(), groupPos);
    plan.appendOperator(move(scanProperty));
}

void Enumerator::appendScanRelPropertyIfNecessary(
    const PropertyExpression& propertyExpression, LogicalPlan& plan) {
    auto& relExpression = (const RelExpression&)*propertyExpression.getChild(0);
    if (!plan.schema->containLogicalExtend(relExpression.getUniqueName())) {
        return;
    }
    auto extend = plan.schema->getExistingLogicalExtend(relExpression.getUniqueName());
    auto scanProperty = make_shared<LogicalScanRelProperty>(extend->boundNodeID,
        extend->boundNodeLabel, extend->nbrNodeID, extend->relLabel, extend->direction,
        propertyExpression.getUniqueName(), propertyExpression.propertyKey, extend->isColumn,
        plan.lastOperator);
    auto groupPos = plan.schema->getGroupPos(extend->nbrNodeID);
    plan.schema->insertToGroupAndScope(propertyExpression.getUniqueName(), groupPos);
    plan.appendOperator(move(scanProperty));
}

vector<unique_ptr<LogicalPlan>> Enumerator::getInitialEmptyPlans() {
    vector<unique_ptr<LogicalPlan>> plans;
    plans.push_back(make_unique<LogicalPlan>());
    return plans;
}

unordered_set<uint32_t> Enumerator::getDependentGroupsPos(
    const shared_ptr<Expression>& expression, const Schema& schema) {
    unordered_set<uint32_t> dependentGroupsPos;
    for (auto& subExpression : getSubExpressionsInSchema(expression, schema)) {
        dependentGroupsPos.insert(schema.getGroupPos(subExpression->getUniqueName()));
    }
    return dependentGroupsPos;
}

vector<shared_ptr<Expression>> Enumerator::getSubExpressionsInSchema(
    const shared_ptr<Expression>& expression, const Schema& schema) {
    vector<shared_ptr<Expression>> results;
    if (schema.expressionInScope(expression->getUniqueName())) {
        results.push_back(expression);
        return results;
    }

    for (auto& child : expression->getChildren()) {
        for (auto& subExpression : getSubExpressionsInSchema(child, schema)) {
            results.push_back(subExpression);
        }
    }
    return results;
}

vector<shared_ptr<Expression>> Enumerator::getSubExpressionsNotInSchemaOfType(
    const shared_ptr<Expression>& expression, const Schema& schema,
    const std::function<bool(ExpressionType)>& typeCheckFunc) {
    vector<shared_ptr<Expression>> results;
    if (schema.expressionInScope(expression->getUniqueName())) {
        return results;
    }
    if (typeCheckFunc(expression->expressionType)) {
        results.push_back(expression);
        return results;
    }
    for (auto& child : expression->getChildren()) {
        for (auto& subExpression :
            getSubExpressionsNotInSchemaOfType(child, schema, typeCheckFunc)) {
            results.push_back(subExpression);
        }
    }
    return results;
}

void Enumerator::computeSchemaForHashJoinAndOrderByAndUnionAll(
    const unordered_set<uint32_t>& groupsToMaterializePos, const Schema& schemaBeforeSink,
    Schema& schemaAfterSink) {
    auto flatVectorsOutputPos = UINT32_MAX;
    auto isAllVectorsFlat = true;
    for (auto& groupToMaterializePos : groupsToMaterializePos) {
        auto groupToMaterialize = schemaBeforeSink.getGroup(groupToMaterializePos);
        if (groupToMaterialize->isFlat) {
            if (flatVectorsOutputPos == UINT32_MAX) {
                flatVectorsOutputPos = schemaAfterSink.createGroup();
            }
            schemaAfterSink.insertToGroupAndScope(
                schemaBeforeSink.getExpressionNamesInScope(groupToMaterializePos),
                flatVectorsOutputPos);
        } else {
            isAllVectorsFlat = false;
            auto groupPos = schemaAfterSink.createGroup();
            schemaAfterSink.insertToGroupAndScope(
                schemaBeforeSink.getExpressionNamesInScope(groupToMaterializePos), groupPos);
            schemaAfterSink.getGroup(groupPos)->estimatedCardinality =
                groupToMaterialize->estimatedCardinality;
        }
    }
    if (!isAllVectorsFlat && flatVectorsOutputPos != UINT32_MAX) {
        schemaAfterSink.flattenGroup(flatVectorsOutputPos);
    }
}

void Enumerator::appendLogicalUnionAll(
    vector<unique_ptr<LogicalPlan>>& childrenPlans, LogicalPlan& logicalPlan) {
    shared_ptr<LogicalUnionAll> logicalUnionAll = make_shared<LogicalUnionAll>();
    for (auto i = 0u; i < childrenPlans.size(); i++) {
        if (i == 0) {
            logicalPlan.setExpressionsToCollect(childrenPlans[i]->getExpressionsToCollect());
            logicalUnionAll->setExpressionsToUnion(logicalPlan.getExpressionsToCollect());
            Enumerator::computeSchemaForHashJoinAndOrderByAndUnionAll(
                childrenPlans[i]->schema->getGroupsPosInScope(), *childrenPlans[i]->schema,
                *logicalPlan.schema);
            logicalPlan.lastOperator = logicalUnionAll;

            // If an expression to union has different flat/unflat state in different child, we
            // need to flatten that expression in all the single queries.
            for (auto j = 0u; j < logicalUnionAll->getExpressionsToUnion().size(); j++) {
                bool hasFlatExpression = false;
                for (auto& childPlan : childrenPlans) {
                    bool isExpressionInChildPlanFlat =
                        childPlan->getSchema()
                            ->getGroup(childPlan->getExpressionsToCollect()[j]->getUniqueName())
                            ->isFlat;
                    hasFlatExpression = hasFlatExpression || isExpressionInChildPlanFlat;
                }
                // If a expression has different flat/unflat state in different childrenPlans, we
                // need to flatten this expression.
                if (hasFlatExpression) {
                    for (auto& childPlan : childrenPlans) {
                        auto expression = childPlan->getExpressionsToCollect()[j];
                        appendFlattenIfNecessary(
                            childPlan->getSchema()->getGroupPos(expression->getUniqueName()),
                            *childPlan);
                    }
                }
            }
        }
        // The returned logicalPlans don't contain the logicalResultCollector as their last
        // operator. We need to append a logicalResultCollector to each returned logicalPlan.
        appendLogicalResultCollector(*childrenPlans[i]);
        logicalUnionAll->addChild(childrenPlans[i]->lastOperator);
    }
}

} // namespace planner
} // namespace graphflow
