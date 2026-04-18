#include "agentcore/state/journal/task_journal.h"

#include <algorithm>
#include <istream>
#include <ostream>
#include <stdexcept>

namespace agentcore {

namespace {

template <typename T>
void write_pod(std::ostream& output, const T& value) {
    output.write(reinterpret_cast<const char*>(&value), static_cast<std::streamsize>(sizeof(T)));
    if (!output) {
        throw std::runtime_error("failed to write serialized task journal");
    }
}

template <typename T>
T read_pod(std::istream& input) {
    T value{};
    input.read(reinterpret_cast<char*>(&value), static_cast<std::streamsize>(sizeof(T)));
    if (!input) {
        throw std::runtime_error("failed to read serialized task journal");
    }
    return value;
}

void write_blob_ref(std::ostream& output, const BlobRef& ref) {
    write_pod(output, ref.pool_id);
    write_pod(output, ref.offset);
    write_pod(output, ref.size);
}

BlobRef read_blob_ref(std::istream& input) {
    return BlobRef{
        read_pod<uint32_t>(input),
        read_pod<uint32_t>(input),
        read_pod<uint32_t>(input)
    };
}

} // namespace

const TaskRecord* TaskJournal::find(InternedStringId key) const noexcept {
    const auto iterator = std::find_if(
        records_.begin(),
        records_.end(),
        [key](const TaskRecord& record) {
            return record.key == key;
        }
    );
    return iterator == records_.end() ? nullptr : std::addressof(*iterator);
}

const std::vector<TaskRecord>& TaskJournal::records() const noexcept {
    return records_;
}

std::size_t TaskJournal::size() const noexcept {
    return records_.size();
}

bool TaskJournal::apply(const std::vector<TaskRecord>& records) {
    bool changed = false;
    for (const TaskRecord& record : records) {
        if (find(record.key) != nullptr) {
            continue;
        }
        records_.push_back(record);
        changed = true;
    }
    return changed;
}

void TaskJournal::clear() noexcept {
    records_.clear();
}

void TaskJournal::serialize(std::ostream& output) const {
    write_pod<uint64_t>(output, static_cast<uint64_t>(records_.size()));
    for (const TaskRecord& record : records_) {
        write_pod(output, record.key);
        write_blob_ref(output, record.request);
        write_blob_ref(output, record.output);
        write_pod(output, record.flags);
    }
}

TaskJournal TaskJournal::deserialize(std::istream& input) {
    TaskJournal journal;
    const uint64_t record_count = read_pod<uint64_t>(input);
    journal.records_.reserve(static_cast<std::size_t>(record_count));
    for (uint64_t index = 0; index < record_count; ++index) {
        journal.records_.push_back(TaskRecord{
            read_pod<InternedStringId>(input),
            read_blob_ref(input),
            read_blob_ref(input),
            read_pod<uint32_t>(input)
        });
    }
    return journal;
}

} // namespace agentcore
