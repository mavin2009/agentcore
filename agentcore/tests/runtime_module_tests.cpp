#include "agentcore/runtime/model_api.h"
#include "agentcore/runtime/scheduler.h"
#include "agentcore/runtime/tool_api.h"
#include "agentcore/state/state_store.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

namespace agentcore {

namespace {

ToolResponse echo_tool(const ToolRequest& request, ToolInvocationContext& context) {
    const std::string output = "tool::" + std::string(context.blobs.read_string(request.input));
    return ToolResponse{true, context.blobs.append_string(output), kToolFlagNone};
}

ModelResponse echo_model(const ModelRequest& request, ModelInvocationContext& context) {
    const std::string output = "model::" + std::string(context.blobs.read_string(request.prompt));
    return ModelResponse{
        true,
        context.blobs.append_string(output),
        0.91F,
        static_cast<uint32_t>(output.size()),
        kModelFlagNone
    };
}

template <typename Predicate>
void wait_until(Predicate predicate) {
    for (int attempts = 0; attempts < 200 && !predicate(); ++attempts) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    assert(predicate());
}

} // namespace

} // namespace agentcore

int main() {
    using namespace agentcore;

    Scheduler scheduler(4);
    std::atomic<int> completed_jobs{0};
    std::vector<std::function<void()>> jobs;
    jobs.reserve(16);
    for (int index = 0; index < 16; ++index) {
        jobs.push_back([&completed_jobs]() {
            completed_jobs.fetch_add(1);
        });
    }
    scheduler.run_batch(jobs);
    assert(completed_jobs.load() == 16);
    assert(scheduler.parallelism() >= 1U);

    scheduler.enqueue_task(ScheduledTask{7U, 11U, 2U, 0U});
    scheduler.enqueue_task(ScheduledTask{7U, 12U, 3U, 10U});
    scheduler.enqueue_task(ScheduledTask{8U, 21U, 1U, 0U});
    assert(scheduler.has_tasks_for_run(7U));
    assert(scheduler.task_count_for_run(7U) == 2U);
    assert(scheduler.task_count_for_run(8U) == 1U);
    assert(scheduler.task_count_for_run(99U) == 0U);
    assert(scheduler.next_task_ready_time_for_run(7U).has_value());
    assert(*scheduler.next_task_ready_time_for_run(7U) == 0U);
    assert(scheduler.next_task_ready_time_for_run(99U) == std::nullopt);

    const auto ready_run7 = scheduler.dequeue_ready_for_run(7U, 0U);
    assert(ready_run7.has_value());
    assert(ready_run7->node_id == 11U);
    assert(scheduler.has_tasks_for_run(7U));
    assert(scheduler.task_count_for_run(7U) == 1U);

    scheduler.remove_run(7U);
    assert(!scheduler.has_tasks_for_run(7U));
    assert(scheduler.task_count_for_run(7U) == 0U);
    assert(scheduler.task_count_for_run(8U) == 1U);

    const auto ready_run8 = scheduler.dequeue_ready_for_run(8U, 0U);
    assert(ready_run8.has_value());
    assert(ready_run8->node_id == 21U);
    assert(!scheduler.has_tasks_for_run(8U));
    assert(scheduler.task_count_for_run(8U) == 0U);

    scheduler.enqueue_task(ScheduledTask{11U, 31U, 1U, 50U});
    scheduler.enqueue_task(ScheduledTask{11U, 32U, 1U, 5U});
    assert(scheduler.next_task_ready_time_for_run(11U).has_value());
    assert(*scheduler.next_task_ready_time_for_run(11U) == 5U);
    assert(!scheduler.has_ready_for_run(11U, 4U));
    const auto delayed_ready = scheduler.dequeue_ready_for_run(11U, 5U);
    assert(delayed_ready.has_value());
    assert(delayed_ready->node_id == 32U);
    scheduler.remove_run(11U);

    scheduler.enqueue_task(ScheduledTask{13U, 41U, 5U, 0U});
    scheduler.enqueue_task(ScheduledTask{13U, 42U, 1U, 0U});
    const auto immediate_first = scheduler.dequeue_ready_for_run(13U, 0U);
    assert(immediate_first.has_value());
    assert(immediate_first->node_id == 41U);
    const auto immediate_second = scheduler.dequeue_ready_for_run(13U, 0U);
    assert(immediate_second.has_value());
    assert(immediate_second->node_id == 42U);

    scheduler.enqueue_task(ScheduledTask{14U, 51U, 1U, 0U});
    scheduler.enqueue_task(ScheduledTask{15U, 61U, 1U, 0U});
    const auto cross_run_first = scheduler.dequeue_ready(0U);
    assert(cross_run_first.has_value());
    assert(cross_run_first->run_id == 14U);
    assert(cross_run_first->node_id == 51U);
    const auto cross_run_second = scheduler.dequeue_ready(0U);
    assert(cross_run_second.has_value());
    assert(cross_run_second->run_id == 15U);
    assert(cross_run_second->node_id == 61U);

    const AsyncWaitKey tool_wait_key{AsyncWaitKind::Tool, 41U};
    scheduler.register_async_waiter(tool_wait_key, ScheduledTask{7, 11, 2, 0U});
    assert(scheduler.has_async_waiters());
    scheduler.signal_async_completion(tool_wait_key);
    assert(scheduler.has_async_waiters_for_run(7U));
    assert(scheduler.promote_ready_async_tasks(0U) == 1U);
    assert(!scheduler.has_async_waiters_for_run(7U));
    const auto promoted_task = scheduler.dequeue_ready_for_run(7U, 0U);
    assert(promoted_task.has_value());
    assert(promoted_task->node_id == 11U);

    const AsyncWaitKey precompleted_model_key{AsyncWaitKind::Model, 77U};
    scheduler.signal_async_completion(precompleted_model_key);
    scheduler.register_async_waiter(precompleted_model_key, ScheduledTask{9, 13, 4, 0U});
    assert(scheduler.promote_ready_async_tasks(0U) == 1U);
    const auto precompleted_task = scheduler.dequeue_ready_for_run(9U, 0U);
    assert(precompleted_task.has_value());
    assert(precompleted_task->node_id == 13U);

    const ScheduledTask multi_wait_task{10U, 15U, 6U, 0U};
    const AsyncWaitKey multi_wait_left{AsyncWaitKind::Tool, 81U};
    const AsyncWaitKey multi_wait_right{AsyncWaitKind::Tool, 82U};
    scheduler.register_async_waiter(multi_wait_left, multi_wait_task);
    scheduler.register_async_waiter(multi_wait_right, multi_wait_task);
    scheduler.signal_async_completion(multi_wait_left);
    assert(scheduler.promote_ready_async_tasks(0U) == 1U);
    scheduler.signal_async_completion(multi_wait_right);
    assert(scheduler.promote_ready_async_tasks(0U) == 0U);
    const auto multi_wait_ready = scheduler.dequeue_ready_for_run(10U, 0U);
    assert(multi_wait_ready.has_value());
    assert(multi_wait_ready->node_id == 15U);
    assert(!scheduler.dequeue_ready_for_run(10U, 0U).has_value());
    scheduler.remove_async_waiters_for_task(*multi_wait_ready);
    assert(!scheduler.has_async_waiters_for_run(10U));
    scheduler.register_async_waiter(multi_wait_right, multi_wait_task);
    scheduler.signal_async_completion(multi_wait_right);
    assert(scheduler.promote_ready_async_tasks(0U) == 1U);
    const auto rearmed_task = scheduler.dequeue_ready_for_run(10U, 0U);
    assert(rearmed_task.has_value());
    assert(rearmed_task->node_id == 15U);
    scheduler.remove_async_waiters_for_task(*rearmed_task);

    const ScheduledTask grouped_wait_task{12U, 17U, 8U, 0U};
    const AsyncWaitKey grouped_wait_left{AsyncWaitKind::Tool, 91U};
    const AsyncWaitKey grouped_wait_right{AsyncWaitKind::Model, 92U};
    scheduler.register_async_wait_group(
        std::vector<AsyncWaitKey>{grouped_wait_left, grouped_wait_right},
        grouped_wait_task
    );
    scheduler.signal_async_completion(grouped_wait_left);
    assert(scheduler.promote_ready_async_tasks(0U) == 0U);
    assert(scheduler.has_async_waiters_for_run(12U));
    scheduler.signal_async_completion(grouped_wait_right);
    assert(scheduler.promote_ready_async_tasks(0U) == 1U);
    const auto grouped_ready = scheduler.dequeue_ready_for_run(12U, 0U);
    assert(grouped_ready.has_value());
    assert(grouped_ready->node_id == 17U);

    StateStore store;
    BlobStore& blobs = store.blobs();
    StringInterner& strings = store.strings();

    ToolRegistry tools;
    std::atomic<uint64_t> completed_tool_handle{0};
    tools.set_async_completion_listener([&completed_tool_handle](AsyncToolHandle handle) {
        completed_tool_handle.store(handle.id);
    });
    tools.register_tool("echo", ToolPolicy{}, echo_tool);
    ToolInvocationContext tool_context{blobs, strings};
    const BlobRef tool_input = blobs.append_string("payload");
    const ToolRequest tool_request{strings.intern("echo"), tool_input};

    const ToolResponse sync_tool = tools.invoke(tool_request, tool_context);
    assert(sync_tool.ok);
    assert(blobs.read_string(sync_tool.output) == "tool::payload");

    const AsyncToolHandle async_tool = tools.begin_invoke_async(tool_request, tool_context);
    wait_until([&completed_tool_handle, async_tool]() {
        return completed_tool_handle.load() == async_tool.id;
    });
    wait_until([&tools, async_tool]() {
        return tools.is_async_ready(async_tool);
    });
    const auto tool_snapshot = tools.export_async_operation(async_tool);
    assert(tool_snapshot.has_value());

    ToolRegistry restored_tools;
    restored_tools.register_tool("echo", ToolPolicy{}, echo_tool);
    const AsyncToolHandle restored_tool = restored_tools.restore_async_operation(*tool_snapshot);
    BlobStore restored_blobs;
    StringInterner restored_strings;
    ToolInvocationContext restored_tool_context{restored_blobs, restored_strings};
    wait_until([&restored_tools, restored_tool]() {
        return restored_tools.is_async_ready(restored_tool);
    });
    const auto async_tool_result = restored_tools.take_async_result(restored_tool, restored_tool_context);
    assert(async_tool_result.has_value());
    assert(async_tool_result->ok);
    assert(restored_blobs.read_string(async_tool_result->output) == "tool::payload");

    ModelRegistry models;
    std::atomic<uint64_t> completed_model_handle{0};
    models.set_async_completion_listener([&completed_model_handle](AsyncModelHandle handle) {
        completed_model_handle.store(handle.id);
    });
    models.register_model("echo-model", ModelPolicy{}, echo_model);
    ModelInvocationContext model_context{blobs, strings};
    const BlobRef prompt = blobs.append_string("plan");
    const BlobRef schema = blobs.append_string("{\"type\":\"string\"}");
    const ModelRequest model_request{strings.intern("echo-model"), prompt, schema, 64U};

    const ModelResponse sync_model = models.invoke(model_request, model_context);
    assert(sync_model.ok);
    assert(blobs.read_string(sync_model.output) == "model::plan");

    const AsyncModelHandle async_model = models.begin_invoke_async(model_request, model_context);
    wait_until([&completed_model_handle, async_model]() {
        return completed_model_handle.load() == async_model.id;
    });
    wait_until([&models, async_model]() {
        return models.is_async_ready(async_model);
    });
    const auto async_model_result = models.take_async_result(async_model, model_context);
    assert(async_model_result.has_value());
    assert(async_model_result->ok);
    assert(blobs.read_string(async_model_result->output) == "model::plan");

    std::cout << "runtime module tests passed" << std::endl;
    return 0;
}
