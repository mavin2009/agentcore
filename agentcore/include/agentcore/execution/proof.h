#ifndef AGENTCORE_PROOF_H
#define AGENTCORE_PROOF_H

#include "agentcore/execution/checkpoint.h"
#include <cstdint>
#include <vector>

namespace agentcore {

struct RunProofDigest {
    uint64_t snapshot_digest{0};
    uint64_t trace_digest{0};
    uint64_t combined_digest{0};
};

[[nodiscard]] RunProofDigest compute_run_proof_digest(
    const RunSnapshot& snapshot,
    const std::vector<TraceEvent>& trace_events
);

[[nodiscard]] RunProofDigest compute_run_proof_digest(
    const CheckpointRecord& record,
    const std::vector<TraceEvent>& trace_events
);

} // namespace agentcore

#endif // AGENTCORE_PROOF_H
