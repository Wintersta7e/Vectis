// SDL2 must be included in the translation unit that defines `main`
// so its `SDL_main.h` macro redefinition takes effect on Windows.
// Without this, Windows linking fails with undefined `SDL_main`.
#include <SDL.h>

#include <cstring>

#include "cli/cli_main.h"
#include "core/app.h"
#include "modes/code/code_mode.h"

int main(int argc, char* argv[])
{
    // CLI dispatch: if the first positional arg is a recognised
    // subcommand or a help flag, run headless and exit without ever
    // touching SDL / OpenGL / ImGui.
    if (argc >= 2) {
        const char* a = argv[1];
        if (std::strcmp(a, "digest") == 0 ||
            std::strcmp(a, "--help") == 0 ||
            std::strcmp(a, "-h") == 0 ||
            std::strcmp(a, "help") == 0)
        {
            return vectis::cli::run(argc, argv);
        }
    }

    vectis::core::App app;
    app.register_mode(std::make_unique<vectis::modes::code::CodeMode>());

    if (!app.initialize()) {
        return 1;
    }

    return app.run();
}
