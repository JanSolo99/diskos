# agents/

Instructions for working on diskOS with an AI coding agent.

`AGENTS.md` in this folder is a ready-made project brief: the hardware facts that are easy to
get wrong, the repo layout, the device-safety rules, and the "verify, do not guess" discipline
that keeps this project honest and the device un-bricked.

## How to use it

- **Claude Code:** copy `agents/AGENTS.md` to `CLAUDE.md` at the repo root (or into
  `~/.claude/`), or start a session and tell Claude to read `agents/AGENTS.md` first.
- **Codex:** copy it to `AGENTS.md` at the repo root, or paste it in at the start of a session.
- **Anything else:** paste the contents in as system/context before you start.

The point is to give the agent the same guardrails we use: check `docs/HARDWARE.md` before
claiming anything about the hardware, never assert device behavior from memory, keep the
fail-closed boot contract intact, and treat a flash as recoverable-but-serious.

## Good first tasks

- Read `docs/HARDWARE.md` and the installer's `README.md`, then ask the agent to explain the
  install flow back to you - a quick check that it has the right mental model.
- Add support/validation for a new stock firmware version (must be flash-tested on real
  hardware before it is declared supported).
- Improve error messages / add error codes for failure modes not yet covered.
