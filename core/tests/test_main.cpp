#include <exception>
#include <iostream>

void run_ranking_tests();
void run_review_queue_tests();
void run_review_session_tests();
void run_fixture_catalog_tests();
void run_protocol_contract_tests();

int main() {
    try {
        run_ranking_tests();
        run_review_queue_tests();
        run_review_session_tests();
        run_fixture_catalog_tests();
        run_protocol_contract_tests();
        std::cout << "All core tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Test failure: " << error.what() << '\n';
        return 1;
    }
}
