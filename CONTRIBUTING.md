# Contributing to naTE

Thank you for your interest in contributing.

## Reporting bugs

Open an issue on [GitHub Issues](https://github.com/lhovind/naTE/issues). Include:

- naTE version (shown in **Help → About**)
- OS and distribution
- Steps to reproduce
- What you expected vs. what happened
- Any relevant log output or screenshots

## Suggesting features

Open an issue with a clear description of the use case. Feature requests with a
concrete "I need to do X and currently can't" framing are much easier to act on
than abstract capability requests.

## Submitting pull requests

1. Fork the repository and create a branch from `dev`.
2. Build and run the full test suite before opening a PR:
   ```bash
   cmake --preset debug
   cmake --build build --target naTE_tests --parallel $(nproc)
   ctest --preset debug
   ```
   All tests must pass.
3. Keep changes focused — one logical change per PR.
4. Write or update tests for any new behaviour.
5. Open the PR against `dev` with a clear description of what changes and why.

## Code style

- C++20, formatted consistently with the surrounding code
- No raw owning pointers — use `unique_ptr`, `shared_ptr`, or stack allocation
- wxWidgets UI code belongs in `src/ui/`; the core layers (`transport/`,
  `session/`, `document/`, `parser/`, `config/`) must not include wx headers

## License

By contributing you agree that your changes will be licensed under the
[GPL v3](LICENSE.md) that covers this project.
