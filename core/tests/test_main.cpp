#include <exception>
#include <filesystem>
#include <iostream>
#include <string_view>

void run_ranking_tests();
void run_review_queue_tests();
void run_review_session_tests();
void run_fixture_catalog_tests();
void run_media_analysis_tests();
void run_preference_model_tests();
void run_link_frames_tests();
void run_link_messages_tests();
void run_link_qr_tests();
void run_link_session_tests();
void write_link_golden_messages(const std::filesystem::path& dir);
void write_link_golden_ik_transcript(const std::filesystem::path& dir);
void run_media_format_tests();
void run_media_meta_tests();
void run_media_kind_tests();
void run_quality_tests();
void run_pdq_tests();
void run_embeddings_tests();
void run_taste_tests();
void run_delete_model_tests();

int main(int argc, char** argv) {
    try {
        // pms_core_tests --write-golden <dir>: regenerate the interop fixtures instead of testing.
        if (argc >= 2 && std::string_view(argv[1]) == "--write-golden") {
            if (argc != 3) {
                std::cerr << "usage: pms_core_tests --write-golden <dir>\n";
                return 2;
            }
            write_link_golden_messages(argv[2]);
            write_link_golden_ik_transcript(argv[2]);
            std::cout << "Golden fixtures written to " << argv[2] << '\n';
            return 0;
        }
        run_ranking_tests();
        run_review_queue_tests();
        run_review_session_tests();
        run_fixture_catalog_tests();
        run_media_analysis_tests();
        run_preference_model_tests();
        run_link_frames_tests();
        run_link_messages_tests();
        run_link_qr_tests();
        run_link_session_tests();
        run_media_format_tests();
        run_media_meta_tests();
        run_media_kind_tests();
        run_quality_tests();
        run_pdq_tests();
        run_embeddings_tests();
        run_taste_tests();
        run_delete_model_tests();
        std::cout << "All core tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Test failure: " << error.what() << '\n';
        return 1;
    }
}
