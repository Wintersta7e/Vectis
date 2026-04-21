#include "cli/cli_main.h"

/// Vectis is a headless CLI tool. The interactive GUI surface was
/// removed on 2026-04-22; every invocation dispatches through
/// `vectis::cli::run`, which handles `digest`, `help`, `--help`,
/// `-h`, and usage errors.
int main(int argc, char* argv[])
{
    return vectis::cli::run(argc, argv);
}
