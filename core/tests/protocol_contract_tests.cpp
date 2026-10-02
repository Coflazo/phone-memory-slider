#include <array>
#include <string>
#include <vector>

#include "pms/fixture_phone_transport.hpp"
#include "test_harness.hpp"

void run_protocol_contract_tests() {
    const std::vector catalog{
        pms::CatalogAsset{"a", 100, false, false},
        pms::CatalogAsset{"favorite", 200, true, false},
        pms::CatalogAsset{"c", 300, false, false},
    };
    pms::FixturePhoneTransport transport{catalog};

    pms_check(!transport.catalog_page("", 2).has_value(), "disconnected catalog access fails");
    transport.connect();

    const auto first = transport.catalog_page("", 2);
    pms_check(first.has_value() && first->assets.size() == 2, "catalog pages respect the requested bound");
    const auto replay = transport.catalog_page("", 2);
    pms_check(replay.has_value() && replay->next_cursor == first->next_cursor, "catalog replay is deterministic");
    pms_check(!transport.catalog_page("0:2", 2).has_value(), "stale cursor is rejected");
    pms_check(!transport.catalog_page("1:2junk", 2).has_value(), "cursor trailing data is rejected");
    pms_check(!transport.catalog_page("", 0).has_value(), "zero page size is rejected");

    const std::array<std::string, 1> favorite_ids{"favorite"};
    const auto protected_batch = transport.prepare_trash(favorite_ids);
    pms_check(!protected_batch.has_value(), "favorite cannot enter remote trash preparation");

    const std::array<std::string, 2> first_batch_ids{"a", "c"};
    const auto prepared = transport.prepare_trash(first_batch_ids);
    pms_check(prepared.has_value() && prepared->total_bytes == 400, "trash preparation reports exact bytes");
    transport.disconnect();
    pms_check(!transport.commit_trash(prepared->token).has_value(), "disconnect before commit cannot mark media trashed");

    transport.connect();
    const std::array<std::string, 1> second_batch_ids{"a"};
    const auto second = transport.prepare_trash(second_batch_ids);
    pms_check(second.has_value(), "trash can be prepared after reconnect");
    const auto committed = transport.commit_trash(second->token);
    const auto committed_replay = transport.commit_trash(second->token);
    pms_check(committed.has_value() && committed->trashed_ids.size() == 1, "commit marks the prepared item");
    pms_check(committed_replay.has_value() && committed_replay->trashed_ids == committed->trashed_ids, "commit replay is idempotent");
}
