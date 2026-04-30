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
2. The plugin version (visible in the dock header, e.g., `v1.0.0e`).
3. Steps to reproduce the problem.
4. Relevant lines from OBS's log file (`Help → Log Files → Show Log Files`). Lines from the plugin are prefixed with `[obs-multistream-plugin]`.
5. Whether the issue happens with the OBS default destination only, with additional destinations only, or both.

### Submitting pull requests

1. **Fork** the repository and create a feature branch:
   ```
   git checkout -b feat/my-change
   ```
2. **Build and test** locally before opening the PR:
   ```powershell
   cmake -S . -B build
   cmake --build build --config Debug
   ctest --test-dir build -C Debug --output-on-failure
   ```
3. **Code style:**
   - C++17, 4-space indentation, opening brace on the same line for control flow and on a new line for function definitions (match `src/plugin-main.cpp`).
   - Keep OBS reference counting strict: every `obs_*_create` / `obs_*_get_*` that returns an owned reference must have a matching `obs_*_release`. Borrowed pointers (e.g., from `obs_frontend_get_streaming_service`) must **not** be released.
   - Prefer per-destination owned encoders/services/outputs; never share the main OBS output's encoders with secondary outputs.
4. **Tests:** if you change validation, normalization, or platform-detection rules in `destination_rules.{h,cpp}`, add or update unit tests in `tests/destination_rules_tests.cpp`.
5. **Commit messages:** use clear, imperative-mood summaries (e.g., `Fix retry epoch invalidation on stop_all`).
6. **Open the PR** against `main` and describe:
   - What problem it solves.
   - How it was tested (build configs, OBS scenarios run).
   - Any new platform-specific rules added.

### Code of conduct

Be respectful and constructive. Keep discussions focused on the code and the project. Personal attacks, harassment, or discriminatory language will not be tolerated.

---

By submitting a contribution, you agree that your contribution will be licensed under the same license as the rest of the project (see `COPYRIGHT`).
