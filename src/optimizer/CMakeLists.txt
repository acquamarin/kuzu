add_library(kuzu_optimizer
        OBJECT
        asp_optimizer.cpp
        factorization_rewriter.cpp
        index_nested_loop_join_optimizer.cpp
        logical_operator_collector.cpp
        logical_operator_visitor.cpp
        optimizer.cpp
        projection_push_down_optimizer.cpp
        remove_factorization_rewriter.cpp
        remove_unnecessary_join_optimizer.cpp)

set(ALL_OBJECT_FILES
        ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:kuzu_optimizer>
        PARENT_SCOPE)
