#include "test_support.hpp"

#include <iostream>

// ajfx Studio's scene model and compiler, headless: every starter scene
// compiles whole and its kernels build, the effect is a function of the scene
// alone, scenes save and load unchanged, and the performance, hands, palette,
// bake and delivery do what they say. The tests are in
// `<area>_tests.cpp`, one file for each part of Studio they are about.
void add_document_tests(std::vector<studio_test::Case>& cases);
void add_compile_tests(std::vector<studio_test::Case>& cases);
void add_guides_tests(std::vector<studio_test::Case>& cases);
void add_bake_tests(std::vector<studio_test::Case>& cases);
void add_stage_pick_tests(std::vector<studio_test::Case>& cases);

#if AJFX_STUDIO_TEST_KERNELS
bool kernels_compile_for_every_starter_scene();
#endif

void add_scene_tests(std::vector<studio_test::Case>& cases);
void add_performance_tests(std::vector<studio_test::Case>& cases);
void add_stage_tests(std::vector<studio_test::Case>& cases);
void add_hands_tests(std::vector<studio_test::Case>& cases);
void add_palette_tests(std::vector<studio_test::Case>& cases);
void add_delivery_tests(std::vector<studio_test::Case>& cases);

int main(int argc, char** argv) {
    // A word on the command line runs only the cases whose names hold it.
    const std::string only = argc > 1 ? argv[1] : "";
    std::vector<studio_test::Case> cases;
    add_document_tests(cases);
    add_compile_tests(cases);
    add_guides_tests(cases);
    add_bake_tests(cases);
    add_stage_pick_tests(cases);
    add_scene_tests(cases);
    add_performance_tests(cases);
    add_stage_tests(cases);
    add_hands_tests(cases);
    add_palette_tests(cases);
    add_delivery_tests(cases);
    int failures = 0;
    for (const studio_test::Case& test : cases) {
        if (!only.empty() && std::string{test.name}.find(only) == std::string::npos) {
            continue;
        }
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
    if (only.empty()) try {
        if (kernels_compile_for_every_starter_scene()) {
            std::cout << "[pass] kernels_compile_for_every_starter_scene\n";
        }
    } catch (const std::exception& error) {
        ++failures;
        std::cout << "[FAIL] kernels_compile_for_every_starter_scene: " << error.what() << '\n';
    }
#endif
    return failures == 0 ? 0 : 1;
}
