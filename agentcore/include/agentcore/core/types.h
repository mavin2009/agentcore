#ifndef AGENTCORE_TYPES_H
#define AGENTCORE_TYPES_H

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace agentcore {

using NodeId = uint32_t;
using EdgeId = uint32_t;
using GraphId = uint32_t;
using StateKey = uint32_t;
using InternedStringId = uint32_t;
using RunId = uint64_t;
using CheckpointId = uint64_t;

struct ExecutionNamespaceRef {
    GraphId graph_id{0};
    NodeId node_id{0};
    std::string session_id;
    uint64_t session_revision{0};
};

struct BlobRef {
    uint32_t pool_id{0};
    uint32_t offset{0};
    uint32_t size{0};

    [[nodiscard]] bool empty() const noexcept {
        return size == 0U;
    }

    bool operator==(const BlobRef& other) const noexcept {
        return pool_id == other.pool_id &&
               offset == other.offset &&
               size == other.size;
    }
};

using Value = std::variant<
    std::monostate,
    int64_t,
    double,
    bool,
    BlobRef,
    InternedStringId
>;

enum class ExecutionStatus : uint8_t {
    NotStarted,
    Running,
    Paused,
    Completed,
    Failed,
    Cancelled
};

struct WorkflowState;
struct ExecutionContext;
struct NodeResult;

struct ExecutionFrame {
    GraphId graph_id{0};
    NodeId current_node{0};
    uint64_t step_index{0};
    CheckpointId checkpoint_id{0};
    uint32_t active_branch_id{0};
    ExecutionStatus status{ExecutionStatus::NotStarted};
};

using NodeExecutorFn = NodeResult(*)(ExecutionContext&);
using ConditionFn = bool(*)(const WorkflowState&);

} // namespace agentcore

#endif // AGENTCORE_TYPES_H
