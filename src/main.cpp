#include "core/app.h"

// Modes will be included as they're implemented:
// #include "modes/code/code_mode.h"
// #include "modes/http/http_mode.h"
// #include "modes/ask/ask_mode.h"
// #include "modes/project/project_mode.h"
// #include "modes/write/write_mode.h"

int main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[])
{
    vectis::core::App app;

    // Register modes — uncomment as they're implemented:
    // app.register_mode(std::make_unique<vectis::modes::CodeMode>());
    // app.register_mode(std::make_unique<vectis::modes::HttpMode>());
    // app.register_mode(std::make_unique<vectis::modes::AskMode>());
    // app.register_mode(std::make_unique<vectis::modes::ProjectMode>());
    // app.register_mode(std::make_unique<vectis::modes::WriteMode>());

    if (!app.initialize()) {
        return 1;
    }

    return app.run();
}
