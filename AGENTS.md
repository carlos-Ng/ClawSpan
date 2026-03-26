# Repository Guidelines

## Project Structure & Module Organization
`daemon-service/` contains the Windows daemon, core services, IPC, security filter, and capability modules. Public C++ headers live in `include/`; VM and vsock code lives in `vmm/`. The WinForms desktop app is in `ui/` targeting `net8.0-windows`. Tests are grouped under `tests/` by module (`common`, `core`, `ipc`, `security/filter`, `vmm`). Runtime configs live in `config/`, release artifacts in `dist/`, and bundled dependencies in `third_party/`. Use `docs/coding-style.md` and `docs/development-standards.md` as the source of truth for repo conventions.

## Build, Test, and Development Commands
Configure once with `cmake -S . -B build -G "Visual Studio 17 2022" -A x64`. Build everything with `cmake --build build --config Debug` or `make`. Package distributables with `cmake --build build --config Release --target dist` or `make dist`; create the release zip with `cmake --build build --config Release --target release`. Run the UI in development mode with `dotnet run --project ui` or `make run-ui`. Optional Python tooling under `tools/` uses `pip install -r tools/requirements.txt`.

## Coding Style & Naming Conventions
C++ uses C++20, tabs for indentation, a 100-column limit, `#pragma once`, and Allman braces for types/functions with attached braces for control statements. Prefer `PascalCase` for types, `lowerCamelCase` for functions, `snake_case` for locals/parameters/files, `member_name_` for fields, and `UPPER_SNAKE_CASE` for constants. New interface types should use the `XxxInterface` suffix, not an `I` prefix. Keep exported headers in `include/`; keep module-private headers beside their implementation.

## Testing Guidelines
Tests are opt-in: configure with `cmake -S . -B build -DBUILD_TESTS=ON`. The project uses GoogleTest via CMake discovery; test executables include `common_tests`, `core_tests`, and module-specific suites under `tests/`. Run all tests with `ctest --test-dir build --build-config Debug --output-on-failure`. Name new test files as `<unit>_test.cc` and place them in the matching module folder.

## Commit & Pull Request Guidelines
Do not push directly to `main`; create branches such as `feat/<name>`, `fix/<name>`, or `docs/<name>`. Recent history mixes older slash-prefixed subjects and Conventional Commits, but new commits should use lowercase Conventional Commit style, for example `feat: add vsock retry handling`. Do not add tool or AI advertising trailers to commits. PRs should describe the change, list verification steps, link related issues, and include screenshots when `ui/` behavior changes.
