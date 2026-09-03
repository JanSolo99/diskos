# Host tests (installer / manager)

Python tests that run on the **build machine**, with no Disc attached. They cover the
parts of the tool that surround the device work — reporting what is connected, and
keeping a restore point you can actually rely on — because those are the parts you
cannot check by plugging something in and watching.

| Test | Run with | Covers |
|---|---|---|
| `managercheck.py` | `python3 tests/managercheck.py` | `diskos_installer.manager`: device reporting in every state (including "cannot enumerate USB", which must never be reported as "no device"), learning the running-mode USB id, and the restore-point lifecycle — save, verify, export, import, and refusing a damaged copy. |
| `diagcheck.py` | `python3 tests/diagcheck.py` | `diskos_installer.diag`: redaction (which must strip home paths *without* mangling prose that happens to contain the username), the run log and its rotation, exception capture, the `TeeReporter`, and the report's contents. Also asserts the property everything else depends on: a log write that fails is swallowed, never raised. |

Both use a scratch state directory via `DISKOS_INSTALLER_HOME`, so they never touch a
real install.

The flashing path itself is deliberately not covered here: it writes to hardware, and
the tool's own approach is to delegate those steps to the proven native binaries rather
than reimplement them. `./diskos-installer doctor` is the check for that side.

The on-device UI has its own host tests under [`../ui/tests/`](../ui/tests/).
