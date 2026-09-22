"""What the architecture requires, independent of what any emulator does.

Keyed by (target register, selector). Each entry is:

  verdict        "ok", or "#XX" for the exception vector we expect
  corroboration  "both"    both references agree with us
                 "qemu" / "bochs"  only that one agrees
                 "neither" we are deliberately stricter than both
  rationale      why, in terms of the architecture rather than the emulator

"neither" is not a bug in the tool. An emulator is a reference, not
silicon, and where the references are permissive we take the manual's
reading: a lax CPU turns a caller's mistake into garbage that surfaces
far from its cause, and — worse for lockstep — garbage that depends on
BIOS contents, which is not reproducible.
"""

EXPECT = {
    # ---- DS: any readable segment, at a DPL the caller can reach
    ("ds", 0x0000): ("ok",  "both",    "null into a non-stack register is allowed; the segment becomes unusable"),
    ("ds", 0x0003): ("ok",  "both",    "null with RPL 3 is still null"),
    ("ds", 0x0008): ("ok",  "both",    "readable code is a legal data segment"),
    ("ds", 0x0010): ("ok",  "both",    "plain read/write data"),
    ("ds", 0x0018): ("ok",  "both",    "data DPL 3, CPL 0, RPL 0: max(CPL,RPL) <= DPL"),
    ("ds", 0x001B): ("ok",  "both",    "data DPL 3, RPL 3: max(0,3) <= 3"),
    ("ds", 0x0020): ("#0B", "both",    "not present -> #NP, error code = selector"),
    ("ds", 0x0028): ("#0D", "both",    "execute-only code is not readable -> #GP"),
    ("ds", 0x0030): ("ok",  "both",    "expand-down data is a legal DS"),
    ("ds", 0x0038): ("ok",  "both",    "byte-granular limit 0FFFh loads as-is"),
    ("ds", 0x0040): ("ok",  "both",    "read-only data is a legal DS; only SS demands writable"),
    ("ds", 0x0080): ("#0D", "both",    "GDT index past the table limit -> #GP, error code = selector"),
    ("ds", 0x0088): ("#0D", "both",    "GDT index past the table limit -> #GP"),
    ("ds", 0x0084): ("#0D", "neither", "TI=1 with LDTR never loaded -> #GP (LDTR is null out of reset)"),
    ("ds", 0x000C): ("#0D", "neither", "TI=1 with LDTR never loaded -> #GP"),

    # ---- SS: stricter than DS on every axis, and its own fault vector
    ("ss", 0x0010): ("ok",  "both",    "writable data at DPL = RPL = CPL is the only thing SS accepts"),
    ("ss", 0x0000): ("#0D", "both",    "null into SS is #GP(0), unlike DS"),
    ("ss", 0x0020): ("#0C", "both",    "a not-present stack segment is #SS, not #NP"),
    ("ss", 0x0028): ("#0D", "both",    "code is never a legal SS"),
    ("ss", 0x0040): ("#0D", "both",    "SS must be writable; read-only data is legal for DS but not SS"),
    ("ss", 0x0018): ("#0D", "both",    "SS requires DPL = CPL; DPL 3 at CPL 0 is #GP even though DS takes it"),
    ("ss", 0x001B): ("#0D", "both",    "SS requires RPL = CPL; error code masks the low three bits (0018, not 001B)"),

    # ---- ES: same rules as DS, spot-checked
    ("es", 0x0020): ("#0B", "both",    "not present -> #NP, as for DS"),
    ("es", 0x0010): ("ok",  "both",    "plain data"),
}
