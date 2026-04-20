#include "agentcore/execution/subgraph/reuse/engine_pool.h"

#include "agentcore/execution/engine.h"

namespace agentcore {

SubgraphChildEnginePool::Lease::Lease(
    SubgraphChildEnginePool* pool,
    std::unique_ptr<ExecutionEngine> engine
) noexcept
    : pool_(pool),
      engine_(std::move(engine)) {}

SubgraphChildEnginePool::Lease::~Lease() {
    if (pool_ != nullptr && engine_ != nullptr) {
        pool_->release(std::move(engine_));
    }
}

SubgraphChildEnginePool::Lease::Lease(Lease&& other) noexcept
    : pool_(other.pool_),
      engine_(std::move(other.engine_)) {
    other.pool_ = nullptr;
}

SubgraphChildEnginePool::Lease& SubgraphChildEnginePool::Lease::operator=(Lease&& other) noexcept {
    if (this != &other) {
        if (pool_ != nullptr && engine_ != nullptr) {
            pool_->release(std::move(engine_));
        }
        pool_ = other.pool_;
        engine_ = std::move(other.engine_);
        other.pool_ = nullptr;
    }
    return *this;
}

ExecutionEngine& SubgraphChildEnginePool::Lease::operator*() const noexcept {
    return *engine_;
}

ExecutionEngine* SubgraphChildEnginePool::Lease::operator->() const noexcept {
    return engine_.get();
}

bool SubgraphChildEnginePool::Lease::valid() const noexcept {
    return engine_ != nullptr;
}

SubgraphChildEnginePool::SubgraphChildEnginePool(SubgraphChildEnginePoolOptions options)
    : options_(options) {}

SubgraphChildEnginePool::~SubgraphChildEnginePool() = default;

SubgraphChildEnginePool::Lease SubgraphChildEnginePool::acquire(ExecutionEngine& provider) {
    std::unique_ptr<ExecutionEngine> engine;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!idle_.empty()) {
            engine = std::move(idle_.back());
            idle_.pop_back();
        }
    }

    if (!engine) {
        engine = std::make_unique<ExecutionEngine>(ExecutionEngineOptions{
            1U,
            true,
            options_.profile
        });
    }

    engine->reset_reusable_subgraph_child_state();
    engine->set_checkpoint_policy(provider.checkpoint_policy());
    engine->borrow_runtime_registries(provider.runtime_tools(), provider.runtime_models());
    engine->borrow_graph_registry(provider);
    return Lease(this, std::move(engine));
}

void SubgraphChildEnginePool::release(std::unique_ptr<ExecutionEngine> engine) {
    if (!engine) {
        return;
    }

    engine->reset_reusable_subgraph_child_state();
    std::lock_guard<std::mutex> lock(mutex_);
    idle_.push_back(std::move(engine));
}

} // namespace agentcore
