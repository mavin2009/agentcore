#ifndef AGENTCORE_EXECUTION_SUBGRAPH_REUSE_ENGINE_POOL_H
#define AGENTCORE_EXECUTION_SUBGRAPH_REUSE_ENGINE_POOL_H

#include "agentcore/execution/checkpoint.h"

#include <memory>
#include <mutex>
#include <vector>

namespace agentcore {

class ExecutionEngine;

struct SubgraphChildEnginePoolOptions {
    ExecutionProfile profile{ExecutionProfile::Balanced};
};

class SubgraphChildEnginePool {
public:
    class Lease {
    public:
        Lease() = default;
        ~Lease();

        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&& other) noexcept;
        Lease& operator=(Lease&& other) noexcept;

        [[nodiscard]] ExecutionEngine& operator*() const noexcept;
        [[nodiscard]] ExecutionEngine* operator->() const noexcept;
        [[nodiscard]] bool valid() const noexcept;

    private:
        friend class SubgraphChildEnginePool;

        Lease(SubgraphChildEnginePool* pool, std::unique_ptr<ExecutionEngine> engine) noexcept;

        SubgraphChildEnginePool* pool_{nullptr};
        std::unique_ptr<ExecutionEngine> engine_;
    };

    explicit SubgraphChildEnginePool(SubgraphChildEnginePoolOptions options = {});
    ~SubgraphChildEnginePool();

    SubgraphChildEnginePool(const SubgraphChildEnginePool&) = delete;
    SubgraphChildEnginePool& operator=(const SubgraphChildEnginePool&) = delete;

    [[nodiscard]] Lease acquire(ExecutionEngine& provider);

private:
    void release(std::unique_ptr<ExecutionEngine> engine);

    SubgraphChildEnginePoolOptions options_{};
    std::mutex mutex_;
    std::vector<std::unique_ptr<ExecutionEngine>> idle_;
};

} // namespace agentcore

#endif // AGENTCORE_EXECUTION_SUBGRAPH_REUSE_ENGINE_POOL_H
