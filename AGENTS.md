# AGENTS.md

C reference implementation of CommonMark (v0.31.1). Pure C99, no external deps.
This is a **fork** with extensions: strikethrough, mark, formula (inline/block), table nodes.

## Build

Out-of-source only (`build/` dir). In-source builds are fatal.

```powershell
# Configure + build (Windows, from repo root)
cmake -S . -B build
cmake --build build

# MSVC/NMAKE alternative
nmake /f Makefile.nmake
```

GNU Make wrapper (Linux/macOS only): `make` / `make test` / `make install`.

## Test

```powershell
# All tests (Windows - MSVC multi-config needs -C flag)
ctest --test-dir build -C Debug --output-on-failure

# Linux/macOS single-config
ctest --test-dir build --output-on-failure
```

Python 3 is required for spec tests. Without it, only `api_test` and `parser_test` (C-based) will run.

Test suites:
- `api_test/` — C/C++ API tests (single binary)
- `parser_test/` — Per-feature C tests (heading, strikethrough, table, link, code, image, block_quote, mark, formula_inline, formula_block)
- `test/` — Python spec conformance, pathological, roundtrip, entity, smart-punct, regression tests
- Shared-lib tests load `libcmark` via Python ctypes at runtime

## Lint & Format

```bash
make lint       # clang-tidy (skips scanners.c), needs clang as CC
make format     # clang-format (LLVM style, sort-includes=0)
```

CI runs `make lint` with clang-tidy on every push.

## Generated files — DO NOT hand-edit

| File | Source | Tool |
|---|---|---|
| `src/scanners.c` | `src/scanners.re` | `re2c` (>= 0.14.2) |
| `src/case_fold.inc` | `data/CaseFolding.txt` | `python3 tools/make_case_fold_inc.py` |
| `src/entities.inc` | — | `python3 tools/make_entities_inc.py` |
| `build/` | — | CMake |

All three `.c`/`.inc` files are committed to reduce build deps. Regenerate only when their sources change.

## Code style

- 2-space indent, spaces (not tabs) for `*.c` / `*.h`
- Tabs for `Makefile`; CRLF for `Makefile.nmake`; LF everywhere else
- LLVM clang-format style with `sort-includes=0`
- Compiler flags: `-Wall -Wextra -pedantic -Wstrict-prototypes` (non-MSVC)
- Debug builds define `CMARK_DEBUG_NODES` for node integrity checks

## Architecture

- `src/cmark.h` — Public API (parsing, AST traversal, rendering). This is the contract.
- `src/main.c` — CLI entry point (`cmark` executable)
- `src/blocks.c` — Block-level parsing
- `src/inlines.c` — Inline-level parsing
- `src/html.c`, `commonmark.c`, `man.c`, `latex.c`, `xml.c` — Renderers
- `src/node.c` — AST node operations
- `src/scanners.re` → `scanners.c` — Regex-based token scanning (re2c)
- `src/buffer.c` — Internal growable buffer

Library target is `cmark` (builds `libcmark`). Executable target is `cmark_exe`.

## Fork-specific extensions

This fork adds node types not in upstream `commonmark/cmark`:
`CMARK_NODE_STRIKETHROUGH`, `CMARK_NODE_MARK`, `CMARK_NODE_FORMULA_INLINE`,
`CMARK_NODE_FORMULA_BLOCK`, `CMARK_NODE_TABLE`, `CMARK_NODE_TABLE_ROW`, `CMARK_NODE_TABLE_CELL`.

When adding new node types, update: `cmark_node_type` enum in `cmark.h`, short-name macros at bottom of `cmark.h`, `CMARK_NODE_LAST_BLOCK`/`CMARK_NODE_LAST_INLINE` sentinel values, all renderers, and `parser_test/` if applicable.

## CMake options

| Option | Default | Effect |
|---|---|---|
| `BUILD_SHARED_LIBS` | `NO` | Shared vs static library |
| `CMARK_LIB_FUZZER` | `OFF` | Build libFuzzer harness |
| `CMAKE_BUILD_TYPE` | `Release` | `Debug`, `Release`, `Profile`, `Asan`, `Ubsan` |
