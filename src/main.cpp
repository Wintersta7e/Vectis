// SDL2 must be included in the translation unit that defines `main`
// so its `SDL_main.h` macro redefinition takes effect on Windows.
// Without this, Windows linking fails with undefined `SDL_main`.
#include <SDL.h>

#include "core/app.h"
#include "modes/code/code_mode.h"

int main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[])
{
    vectis::core::App app;

    app.register_mode(std::make_unique<vectis::modes::code::CodeMode>());

    if (!app.initialize()) {
        return 1;
    }

    return app.run();
}
