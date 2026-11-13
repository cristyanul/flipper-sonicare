v1.1 - 2026-11-13:
- Fix: Correct 10-byte MFG string display and NFC unlock password derivation in Read Complete scene
- Fix: Perform complete factory reset by also zeroing session counter and last-brush timestamp (pages 0x25 & 0x26)
- Add: Quick Reset (Auto) mode for rapid 1-tap batch resetting of multiple brush heads
- Add: Strict AUTHLIM pre-check to prevent sending invalid passwords to the chip

v1.0 - 2026-08-03:
- Add: Reset usage counter (with AUTHLIM warning dialog)
  Thanks to Michael Huber

v0.9 - 2026-03-31:
Initial Release
