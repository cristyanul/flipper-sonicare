v1.1 - 2026-11-13:
- Fix: Authenticate with derived password in ReadSuccess before writing (resolves write failure on write-protected PROT=0 heads)
- Fix: Correct 10-byte MFG string display across pages 0x21-0x23 and fix password derivation in Read Complete scene
- Fix: Perform complete factory reset by also zeroing session counter (page 0x25) and last-brush timestamp (page 0x26)

v1.0 - 2026-08-03:
- Add: Reset usage counter (with AUTHLIM warning dialog)
  Thanks to Michael Huber

v0.9 - 2026-03-31:
Initial Release
