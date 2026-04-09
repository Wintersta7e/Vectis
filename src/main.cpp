// SDL2 must be included in the translation unit that defines `main`
// so its `SDL_main.h` macro redefinition takes effect on Windows.
// Without this, Windows linking fails with undefined `SDL_main`.
#include <SDL.h>

#include "core/app.h"
#include "modes/code/code_mode.h"

// Further modes will be included as they're implemented:
// #include "modes/http/http_mode.h"
// #include "modes/ask/ask_mode.h"
// #include "modes/project/project_mode.h"
// #include "modes/write/write_mode.h"

int main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[])
{
    vectis::core::App app;

    app.register_mode(std::make_unique<vectis::modes::code::CodeMode>());
    // Further modes — register as they come online:
    // app.register_mode(std::make_unique<vectis::modes::HttpMode>());
    // app.register_mode(std::make_unique<vectis::modes::AskMode>());
    // app.register_mode(std::make_unique<vectis::modes::ProjectMode>());
    // app.register_mode(std::make_unique<vectis::modes::WriteMode>());

    if (!app.initialize()) {
        return 1;
    }

    return app.run();
}
