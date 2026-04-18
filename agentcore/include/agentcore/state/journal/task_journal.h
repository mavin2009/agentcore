#ifndef AGENTCORE_TASK_JOURNAL_H
#define AGENTCORE_TASK_JOURNAL_H

#include "agentcore/core/types.h"
#include <cstddef>
#include <iosfwd>
#include <vector>

namespace agentcore {

struct TaskRecord {
    InternedStringId key{0};
    BlobRef request{};
    BlobRef output{};
    uint32_t flags{0};
};

class TaskJournal {
public:
    [[nodiscard]] const TaskRecord* find(InternedStringId key) const noexcept;
    [[nodiscard]] const std::vector<TaskRecord>& records() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool apply(const std::vector<TaskRecord>& records);
    void clear() noexcept;
    void serialize(std::ostream& output) const;
    [[nodiscard]] static TaskJournal deserialize(std::istream& input);

private:
    std::vector<TaskRecord> records_;
};

} // namespace agentcore

#endif // AGENTCORE_TASK_JOURNAL_H
