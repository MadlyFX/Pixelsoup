#include "studio_app.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

int main(int argc, char** argv) {
    ajfx::studio::StudioOptions options;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            std::puts("ajfx-studio [page.ajfxstudio] [--recipe <id>] [--expert] [--open-blocks <kind|all>] [--picker] [--at <seconds>] [--once] [--try <name=value>] [--paths] [--no-guides] [--fresh] [--maximize] [--gpu-debug] [--lose-device <seconds>]\n"
                      "Authors deterministic scenes and exports .ajfx packages. --recipe opens optional legacy reference content.");
            return 0;
        }
        const std::string argument = argv[i];
        if (argument == "--recipe" && i + 1 < argc) {
            options.recipe = argv[++i];
            options.restore_page = false;
        } else if (argument == "--fresh") {
            options.fresh = true;
            options.restore_page = false;
        } else if (argument == "--expert") {
            options.expert = true;
        } else if (argument == "--open-blocks" && i + 1 < argc) {
            options.open_blocks = argv[++i];
        } else if (argument == "--once") {
            options.once = true;
        } else if (argument == "--picker") {
            options.picker = true;
        } else if (argument == "--maximize") {
            options.maximize = true;
        } else if (argument == "--gpu-debug") {
            options.gpu_debug = true;
        } else if (argument == "--lose-device" && i + 1 < argc) {
            options.lose_device = std::strtof(argv[++i], nullptr);
        } else if (argument == "--paths") {
            options.paths = true;
        } else if (argument == "--no-guides") {
            options.guides = false;
        } else if (argument == "--try" && i + 1 < argc) {
            options.tries.emplace_back(argv[++i]);
        } else if (argument == "--at" && i + 1 < argc) {
            options.at = std::strtof(argv[++i], nullptr);
        } else {
            options.open = argument;
            options.restore_page = false;
        }
    }
    return ajfx::studio::run_studio(options);
}
