#include "studio_app.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

int main(int argc, char** argv) {
    ajfx::studio::StudioOptions options;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            std::puts("ajfx-studio [scene.ajfxstudio] [--at <seconds>] [--once] [--paths] [--no-guides] [--fresh] [--gpu-debug] [--lose-device <seconds>]\n"
                      "Authors deterministic scenes and exports .ajfx packages.");
            return 0;
        }
        const std::string argument = argv[i];
        if (argument == "--fresh") {
            options.fresh = true;
            options.restore_page = false;
        } else if (argument == "--once") {
            options.once = true;
        } else if (argument == "--gpu-debug") {
            options.gpu_debug = true;
        } else if (argument == "--lose-device" && i + 1 < argc) {
            options.lose_device = std::strtof(argv[++i], nullptr);
        } else if (argument == "--paths") {
            options.paths = true;
        } else if (argument == "--no-guides") {
            options.guides = false;
        } else if (argument == "--at" && i + 1 < argc) {
            options.at = std::strtof(argv[++i], nullptr);
        } else {
            options.open = argument;
            options.restore_page = false;
        }
    }
    return ajfx::studio::run_studio(options);
}
