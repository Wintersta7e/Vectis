# Vectis Code Mode Test Fixtures

Minimal, self-contained sample projects in each supported language.
Used as a testing ground for the Code mode scanner, parser, and UI:

- **Manual smoke testing** — open any `sample-*/` directory in Vectis
  and verify file tree, symbol browser, and code viewer behave correctly.
- **Automated testing** — `tests/code/fixtures_test.cpp` scans
  `sample-python/` and asserts expected files, symbols, and kinds.

Each fixture is deliberately tiny (a handful of files, <100 LOC total)
but contains enough different symbol kinds to exercise the parser:
functions, classes/structs, interfaces/traits, enums, methods, and
namespaces where the language supports them.

When running Vectis on the Vectis repo itself, these fixtures will
show up in the file tree — that is expected and useful for manual
verification.
