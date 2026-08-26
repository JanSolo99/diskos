"""Stable, user-quotable error codes for the diskOS installer.

Every user-facing failure carries a short code a beta tester can quote from a
screenshot so we can triage it instantly. Namespaces:

  E1xx  environment / preflight  (nothing written to the device yet)
  E2xx  firmware extract & image build
  E3xx  flash orchestration (host side)
  F1xx  device writer result (mirrors the on-device dbg[16] codes)

An exception's str() renders as:  "[E301] <what happened> - <what to do>"
so existing `rep.error(str(e))` call sites show the code with no other change.
"""


class DiskOSError(Exception):
    """Base installer error carrying a stable `code` and an optional `action`
    (a short, plain-language "what to do next")."""
    code = "E000"

    def __init__(self, message, code=None, action=None):
        self.message = message
        if code:
            self.code = code
        self.action = action
        super().__init__(self.render())

    def render(self):
        s = f"[{self.code}] {self.message}"
        if self.action:
            s += f" - {self.action}"
        return s


class PreflightError(DiskOSError):
    """E1xx - environment/preflight; the device has NOT been touched."""
    code = "E100"


class BuildError(DiskOSError):
    """E2xx - firmware extraction / diskOS image build."""
    code = "E200"


class FlashError(DiskOSError):
    """E3xx - flash orchestration (host side). F0xx = device-writer verdicts."""
    code = "E300"


# Device-writer result codes (from my_write5.c dbg[16]) → stable F-codes for the user.
DEVICE_RESULT_CODES = {
    0x600DF10C: ("F001", "SUCCESS"),
    0xDEAD0001: ("F101", "ABORT init/ECC"),
    0xDEAD0002: ("F102", "ABORT out-of-space"),
    0xDEAD0003: ("F103", "ABORT persistent block-write fail"),
    0xDEAD0004: ("F104", "ABORT too many bad blocks"),
    0xDEAD0005: ("F105", "ABORT ECC re-enable failed"),
    0xDEAD0006: ("F106", "ABORT bad-block marker unreadable"),
}

RECOVERABLE = ("The device is normally recoverable via mask-ROM (not guaranteed for every unit "
               "or failure): power the device OFF, hold Volume-Down, plug in USB to return to "
               "mask-ROM, and re-flash your saved stock image or diskOS.")
