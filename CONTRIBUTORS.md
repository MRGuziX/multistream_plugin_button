# Contributors

Thank you to everyone who has contributed to **obs-multistream-plugin**!

## Maintainers

- **Tomasz Guzik** ([@MRGuziX](https://github.com/MRGuziX)) — `tomasz.guzik@onet.eu` — author and current maintainer.

## Contributors

<!--
Add yourself in the form:
- **Your Name** ([@your-github-handle](https://github.com/your-github-handle)) — short note about your contribution.
-->

_No external contributors yet — be the first!_

---

## How to contribute

We welcome bug reports, feature requests, and pull requests.

### Reporting issues

When opening an issue on GitHub, please include:

1. OBS Studio version (e.g., `30.2.3`) and your OS (Windows 10 / 11, build).
2. The plugin version (visible in the dock header, e.g., `v1.1.7` for a release build, or a `git describe` string for local builds).
3. Steps to reproduce the problem.
4. Relevant lines from OBS's log file (`Help → Log Files → Show Log Files`). Lines from the plugin are prefixed with `[obs-multistream-plugin]`.
5. Whether the issue happens with the OBS default destination only, with additional destinations only, or both.

### Submitting pull requests

1. **Fork** the repository and create a feature branch:
   ```
   git checkout -b feat/my-change
   ```
2. **Build and test** locally before opening the PR. `cmake -S . -B build` only works after **libobs** (and related packages) are discoverable — follow **Build from source** in `README.md` (submodule, OBS install including **Development**, then `CMAKE_PREFIX_PATH` / optional `libobs_DIR`). Then, for example:
   ```powershell
   cmake -S . -B build
   cmake --build build --config Debug
   ```
   On Windows, extend **`PATH`** with the same `bin` directories used for a dev OBS install (OBS, Qt, obs-deps) before **`ctest`**, or DLL resolution may fail:
   ```powershell
   ctest --test-dir build -C Debug --output-on-failure
   ```
3. **Code style:**
   - C++17, 4-space indentation, opening brace on the same line for control flow and on a new line for function definitions (match existing conventions in `src/`).
   - Keep OBS reference counting strict: every `obs_*_create` / `obs_*_get_*` that returns an owned reference must have a matching `obs_*_release`. Borrowed pointers (e.g., from `obs_frontend_get_streaming_service`) must **not** be released.
   - Prefer per-destination owned encoders/services/outputs; never share the main OBS output's encoders with secondary outputs.
4. **Tests:** if you change `destination_rules.{h,cpp}`, extend `tests/destination_rules_tests.cpp`. If you change `config_io.{h,cpp}`, extend `tests/config_io_tests.cpp`. Add or adjust stubs in `tests/` only when the test target needs a smaller surface than the full plugin.
5. **Commit messages:** use clear, imperative-mood summaries (e.g., `Fix retry epoch invalidation on stop_all`).
6. **Open the PR** against `master` (the repository default branch) and describe:
   - What problem it solves.
   - How it was tested (build configs, OBS scenarios run).
   - Any new platform-specific rules added.

### Code of conduct

Be respectful and constructive. Keep discussions focused on the code and the project. Personal attacks, harassment, or discriminatory language will not be tolerated.

---

By submitting a contribution, you agree that your contribution will be licensed under the same license as the rest of the project (see `COPYRIGHT`).
