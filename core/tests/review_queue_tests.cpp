#include <optional>
#include <string>
#include <vector>

#include "pms/review_queue.hpp"
#include "test_harness.hpp"

using pms::Decision;
using pms::ReviewQueue;

void run_review_queue_tests() {
    {
        ReviewQueue queue;
        pms_check(queue.decide("asset-1", Decision::Delete, 1'024), "first decision changes state");
        pms_check(!queue.decide("asset-1", Decision::Delete, 1'024), "duplicate decision is idempotent");
        pms_check(queue.pending_delete_count() == 1, "delete count is not duplicated");
        pms_check(queue.pending_delete_bytes() == 1'024, "delete bytes are not duplicated");
        const auto undone = queue.undo();
        pms_check(undone.has_value() && undone->asset_id == "asset-1", "undo returns the affected asset");
        pms_check(queue.pending_delete_count() == 0 && queue.pending_delete_bytes() == 0, "undo restores aggregates");
    }

    {
        ReviewQueue queue;
        queue.protect("favorite");
        queue.protect("kept");
        pms_check(!queue.decide("favorite", Decision::Delete, 100), "favorite cannot be deleted");
        pms_check(!queue.decide("kept", Decision::Delete, 200), "explicit keep cannot be deleted");
        pms_check(queue.decide("candidate", Decision::Delete, 300), "unprotected candidate can be queued");
        const auto batch = queue.prepare_trash_batch();
        pms_check(batch.asset_ids.size() == 1 && batch.asset_ids.front() == "candidate", "batch excludes protected assets");
        pms_check(batch.total_bytes == 300, "batch totals eligible bytes only");
    }

    {
        ReviewQueue queue;
        pms_check(queue.decide("asset", Decision::Delete, 500), "delete changes state");
        pms_check(queue.decide("asset", Decision::Keep, 500), "changing to keep changes state");
        pms_check(queue.pending_delete_count() == 0 && queue.pending_delete_bytes() == 0, "changed decision updates aggregates once");
    }
}
