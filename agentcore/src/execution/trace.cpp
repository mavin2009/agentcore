#include "agentcore/execution/checkpoint.h"

namespace agentcore {

void TraceSink::emit(const TraceEvent& event) {
    std::lock_guard<std::mutex> lock(mutex_);
    TraceEvent stored = event;
    stored.sequence = next_sequence_++;
    events_.push_back(std::move(stored));
}

const std::vector<TraceEvent>& TraceSink::events() const noexcept {
    return events_;
}

std::vector<TraceEvent> TraceSink::events_for_run(RunId run_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<TraceEvent> events;
    for (const TraceEvent& event : events_) {
        if (event.run_id == run_id) {
            events.push_back(event);
        }
    }
    return events;
}

std::vector<TraceEvent> TraceSink::events_for_run_since_sequence(
    RunId run_id,
    uint64_t next_sequence
) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<TraceEvent> events;
    for (const TraceEvent& event : events_) {
        if (event.sequence < next_sequence || event.run_id != run_id) {
            continue;
        }
        events.push_back(event);
    }
    return events;
}

uint64_t TraceSink::next_sequence() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return next_sequence_;
}

void TraceSink::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    events_.clear();
    next_sequence_ = 1U;
}

} // namespace agentcore
