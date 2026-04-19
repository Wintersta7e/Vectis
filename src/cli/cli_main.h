#pragma once

namespace vectis::cli {

/// Entry point for Vectis's headless CLI.
///
/// `main` dispatches to this when the first positional argument is a
/// recognised subcommand (`digest`, `--help`). The CLI path deliberately
/// never pulls in SDL / OpenGL / ImGui so it can run in CI and other
/// display-less environments.
///
/// Returns a shell exit code: 0 on success, 1 on user error (bad args,
/// missing path), 2 on scan / export failure.
[[nodiscard]] int run(int argc, char** argv);

} // namespace vectis::cli
