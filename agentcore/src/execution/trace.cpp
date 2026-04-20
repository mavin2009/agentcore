#include "agentcore/execution/checkpoint.h"

#include <algorithm>

namespace agentcore {

void TraceSink::configure(ExecutionProfile profile, std::size_t fast_limit) {
    std::lock_guard<std::mutex> lock(mutex_);
    bounded_ = (profile == ExecutionProfile::Fast);
    max_events_per_run_ = bounded_ ? std::max<std::size_t>(1U, fast_limit) : 0U;
}

void TraceSink::trim_run_locked(RunTrace& run_trace) {
    if (!bounded_ || max_events_per_run_ == 0U) {
        return;
    }

    while (run_trace.event_count > max_events_per_run_ && !run_trace.segments.empty()) {
        std::vector<TraceEvent>& front_segment = run_trace.segments.front();
        if (front_segment.empty()) {
            run_trace.segments.pop_front();
            continue;
        }

        front_segment.erase(front_segment.begin());
        run_trace.event_count -= 1U;
        if (front_segment.empty()) {
            run_trace.segments.pop_front();
        }
    }
}

void TraceSink::emit(const TraceEvent& event) {
    std::lock_guard<std::mutex> lock(mutex_);
    TraceEvent stored = event;
    stored.sequence = next_sequence_++;

    RunTrace& run_trace = events_by_run_[stored.run_id];
    if (run_trace.segments.empty() || run_trace.segments.back().size() >= kSegmentSize) {
        run_trace.segments.emplace_back();
        run_trace.segments.back().reserve(kSegmentSize);
    }
    run_trace.segments.back().push_back(std::move(stored));
    run_trace.event_count += 1U;
    trim_run_locked(run_trace);
}

void TraceSink::emit_batch(std::vector<TraceEvent> events) {
    if (events.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    for (TraceEvent& event : events) {
        event.sequence = next_sequence_++;

        RunTrace& run_trace = events_by_run_[event.run_id];
        if (run_trace.segments.empty() || run_trace.segments.back().size() >= kSegmentSize) {
            run_trace.segments.emplace_back();
            run_trace.segments.back().reserve(kSegmentSize);
        }
        run_trace.segments.back().push_back(std::move(event));
        run_trace.event_count += 1U;
        trim_run_locked(run_trace);
    }
}

std::vector<TraceEvent> TraceSink::events() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<TraceEvent> events;
    for (const auto& [_, run_trace] : events_by_run_) {
        for (const std::vector<TraceEvent>& segment : run_trace.segments) {
            events.insert(events.end(), segment.begin(), segment.end());
        }
    }
    std::sort(events.begin(), events.end(), [](const TraceEvent& left, const TraceEvent& right) {
        return left.sequence < right.sequence;
    });
    return events;
}

std::vector<TraceEvent> TraceSink::events_for_run(RunId run_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<TraceEvent> events;
    const auto iterator = events_by_run_.find(run_id);
    if (iterator == events_by_run_.end()) {
        return events;
    }

    events.reserve(iterator->second.event_count);
    for (const std::vector<TraceEvent>& segment : iterator->second.segments) {
        events.insert(events.end(), segment.begin(), segment.end());
    }
    return events;
}

std::vector<TraceEvent> TraceSink::take_events_for_run(RunId run_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<TraceEvent> events;
    auto iterator = events_by_run_.find(run_id);
    if (iterator == events_by_run_.end()) {
        return events;
    }

    events.reserve(iterator->second.event_count);
    for (std::vector<TraceEvent>& segment : iterator->second.segments) {
        events.insert(
            events.end(),
            std::make_move_iterator(segment.begin()),
            std::make_move_iterator(segment.end())
        );
    }
    events_by_run_.erase(iterator);
    return events;
}

std::vector<TraceEvent> TraceSink::events_for_run_since_sequence(
    RunId run_id,
    uint64_t next_sequence
) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<TraceEvent> events;
    const auto iterator = events_by_run_.find(run_id);
    if (iterator == events_by_run_.end()) {
        return events;
    }

    for (const std::vector<TraceEvent>& segment : iterator->second.segments) {
        for (const TraceEvent& event : segment) {
            if (event.sequence >= next_sequence) {
                events.push_back(event);
            }
        }
    }
    return events;
}

uint64_t TraceSink::next_sequence() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return next_sequence_;
}

void TraceSink::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    events_by_run_.clear();
    next_sequence_ = 1U;
}

} // namespace agentcore
