# Contributors

Thank you to everyone who has contributed to **obs-multistream-plugin**!

## Maintainers

- **Tomasz Guzik** ([@MRGuziX](https://github.com/MRGuziX)) — `tomasz.guzik@onet.eu` — author and current maintainer.

## Contributors

- **Claude** (Anthropic) — AI-assisted development: architecture, encoder sharing, CI/CD, and code review.

---

## How to contribute

We welcome bug reports, feature requests, and pull requests.

### Reporting issues

When opening an issue on GitHub, please include:

1. OBS Studio version (e.g., `30.2.3`) and your OS (Windows 10 / 11, build).
2. The plugin version (visible in the dock header).
3. Steps to reproduce the problem.
4. Relevant lines from OBS's log file (`Help → Log Files → Show Log Files`). Lines from the plugin are prefixed with `[obs-multistream-plugin]`.
5. Whether the issue happens with the OBS default destination only, with additional destinations only, or both.

### Submitting pull requests

1. **Fork** the repository and create a feature branch:
   ```
   git checkout -b feat/my-change
   ```
2. **Build and test** locally before opening the PR. Follow the **Build from source** section in `README.md` — you need the OBS submodule built with the Development component before `cmake -S . -B build` will work.
3. **Code style:**
   - C++17, 4-space indentation. Match existing conventions in `src/`.
   - Keep OBS reference counting strict: every `obs_*_create` / `obs_*_get_*` that returns an owned reference must have a matching `obs_*_release`.
   - When sharing the main stream's encoder, use `obs_encoder_get_ref()` to hold a reference and release it on cleanup.
4. **Tests:** extend `tests/destination_rules_tests.cpp` for validation/normalization changes, `tests/config_io_tests.cpp` for config changes.
5. **Commit messages:** use clear, imperative-mood summaries.
6. **Open the PR** and describe what problem it solves and how it was tested.

### Code of conduct

Be respectful and constructive. Keep discussions focused on the code and the project.

---

By submitting a contribution, you agree that your contribution will be licensed under the same license as the rest of the project (see `COPYRIGHT`).
