# Contributing

Bug reports, board reports and pull requests are welcome.

- **Board report:** the most useful contribution. Use the *Board report* issue template and include the `device.board` block from
  `GET /api/status`.
- **Build and test** as in [docs/BUILDING.md](docs/BUILDING.md). A firmware change should pass `tests/run.sh`; a UI change should pass
  `npx tsc --noEmit` and `npm run build`.
- **Style:** match the surrounding code (the firmware is C++17, the UI is Preact + TypeScript). Pure logic gets a check in
  `picocam/firmware/tests/selftest.cc`.
- **Docs** live in `docs/` and `README.md`; the wiki is generated from them (do not edit the wiki directly). Keep the tone plain and the steps checkable.
- **Never commit** real footage, plates, faces, credentials or binaries.
- Keep the event loop non-blocking: nothing on the loop thread may block for more than about a millisecond.
