#include "test_support.hpp"

#include <iostream>

// ajfx Studio's page model and compiler, headless: every recipe builds a valid
// effect within its budget, block order and unrelated edits leave an effect
// alone, page scale scales, what blocks share is merged, pages save and load
// unchanged, and the coach's fixes fix what it points at. The tests are in
// `<area>_tests.cpp`, one file for each part of Studio they are about.
void add_stream_object_tests(std::vector<studio_test::Case>& cases);
void add_catalog_tests(std::vector<studio_test::Case>& cases);
void add_document_tests(std::vector<studio_test::Case>& cases);
void add_compile_tests(std::vector<studio_test::Case>& cases);
void add_rigid_objects_tests(std::vector<studio_test::Case>& cases);
void add_rigid_break_tests(std::vector<studio_test::Case>& cases);
void add_guides_tests(std::vector<studio_test::Case>& cases);
void add_bake_tests(std::vector<studio_test::Case>& cases);
void add_world_tests(std::vector<studio_test::Case>& cases);
void add_solid_world_tests(std::vector<studio_test::Case>& cases);
void add_stage_pick_tests(std::vector<studio_test::Case>& cases);

#if AJFX_STUDIO_TEST_KERNELS
bool kernels_compile_for_every_recipe();
#endif

void add_scene_tests(std::vector<studio_test::Case>& cases);
void add_performance_tests(std::vector<studio_test::Case>& cases);
void add_stage_tests(std::vector<studio_test::Case>& cases);
void add_hands_tests(std::vector<studio_test::Case>& cases);
void add_delivery_tests(std::vector<studio_test::Case>& cases);

int main() {
    std::vector<studio_test::Case> cases;
    add_stream_object_tests(cases);
    add_catalog_tests(cases);
    add_document_tests(cases);
    add_compile_tests(cases);
    add_rigid_objects_tests(cases);
    add_rigid_break_tests(cases);
    add_guides_tests(cases);
    add_bake_tests(cases);
    add_world_tests(cases);
    add_solid_world_tests(cases);
    add_stage_pick_tests(cases);
    add_scene_tests(cases);
    add_performance_tests(cases);
    add_stage_tests(cases);
    add_hands_tests(cases);
    add_delivery_tests(cases);
    int failures = 0;
    for (const studio_test::Case& test : cases) {
        try {
            test.run();
            // Flushed, so a crash still shows the last test that passed.
            std::cout << "[pass] " << test.name << std::endl;
        } catch (const std::exception& error) {
            ++failures;
            std::cout << "[FAIL] " << test.name << ": " << error.what() << '\n';
        }
    }
#if AJFX_STUDIO_TEST_KERNELS
    try {
        if (kernels_compile_for_every_recipe()) {
            std::cout << "[pass] kernels_compile_for_every_recipe\n";
        }
    } catch (const std::exception& error) {
        ++failures;
        std::cout << "[FAIL] kernels_compile_for_every_recipe: " << error.what() << '\n';
    }
#endif
    return failures == 0 ? 0 : 1;
}
