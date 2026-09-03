# Engineering Principles
- **Minimal & Focused**: Start with the smallest working change that solves the problem. Add complexity only when proven necessary.
- **Direct & Audit-friendly**: Prefer straightforward control flow and clear types over speculative abstractions or generic frameworks.
- **DRY & Standard-first**: Reuse established std/library facilities. Keep logic deduplicated and functional naming in clear `verbObject` form.
- **Clean Separation**: Pure decision logic in testable units; keep Qt, IPC, I/O, and worker calls in thin adapters.
- **Zero Dead Code**: Delete replaced code and unused includes/variables in the same commit. Add comments for clarity.

# Task Engineering Lifecycle
1. **Scope**: Identify the smallest correct fix. Avoid speculative refactors.
2. **Implement**: Keep types narrow and adapters thin.
3. **Validate**: Add table-driven unit tests for new behavior. Run `ctest` across all suites.
4. **Audit for Regression**: Review `git diff` for dead code, unused headers, unintended side effects, or style violations.
5. **Format & Clean**: Format with `clang-format` (`./scripts/build.sh format`) before finalizing.

# Build & Test Quick Reference
```bash
# Build & run all tests (Debug with Clang)
./scripts/build.sh dev

# Helper script presets
./scripts/build.sh dev          # Debug build + run tests
./scripts/build.sh release      # Optimized Release build
./scripts/build.sh format       # Run clang-format on src/ and tests/

# Targeted test execution
ctest --test-dir build -R <test_pattern> --output-on-failure   # Run specific test by name
ctest --test-dir build -L <label> --output-on-failure          # Run by label (security, worker, generator, etc.)
ctest --test-dir build -LE slow --output-on-failure            # Run all tests excluding the (unused) slow label
```

# Architecture Map
- `src/generator/`: Okular Generator UI plugin & settings (KDE / KF6 / Okular SDK).
- `src/plugin/`: Pure-Qt intermediary host bridge (IPC client, NSS crypto, OCR scheduler, utilities).
- `src/worker/`: Out-of-process sandboxed worker engine (MuPDF, OpenSSL, native C++, POSIX; no Qt/GUI).
- `src/shared/`: Cross-layer data models, binary zpp::bits serialization, and transport abstractions.
- `tests/`: Layered test suites corresponding to each layer (`generator/`, `plugin/`, `worker/`, `security/`, `integration/`, `corpus/`, `benchmark/`).
