#include <exception>
#include <iostream>

void run_ranking_tests();
void run_review_queue_tests();
void run_review_session_tests();
void run_fixture_catalog_tests();
void run_media_analysis_tests();
void run_preference_model_tests();
void run_media_format_tests();
void run_media_meta_tests();
void run_media_kind_tests();
void run_quality_tests();
void run_pdq_tests();
void run_embeddings_tests();

int main() {
    try {
        run_ranking_tests();
        run_review_queue_tests();
        run_review_session_tests();
        run_fixture_catalog_tests();
        run_media_analysis_tests();
        run_preference_model_tests();
        run_media_format_tests();
        run_media_meta_tests();
        run_media_kind_tests();
        run_quality_tests();
        run_pdq_tests();
        run_embeddings_tests();
        std::cout << "All core tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Test failure: " << error.what() << '\n';
        return 1;
    }
}
