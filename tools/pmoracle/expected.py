"""What the architecture requires, independent of what any emulator does.

Each entry is (verdict, corroboration, rationale).

  verdict        "ok", or "#XX" for the exception vector we expect
  corroboration  "both"    both references agree with us
                 "qemu" / "bochs"  only that one agrees
                 "neither" we are deliberately stricter than both

"neither" is not a bug in the tool. An emulator is a reference, not
silicon, and where the references are permissive we take the manual's
reading: a lax CPU turns a caller's mistake into garbage that surfaces
far from its cause, and — worse for us — garbage that depends on BIOS
contents, which is not reproducible. Lockstep needs determinism.
"""

EXPECT = {
    0x0000: ("ok",  "both",    "null selector into DS is allowed; the segment becomes unusable"),
    0x0003: ("ok",  "both",    "null selector, RPL 3: still null, still allowed"),
    0x0008: ("ok",  "both",    "readable code segment is a legal DS"),
    0x0010: ("ok",  "both",    "plain read/write data"),
    0x0018: ("ok",  "both",    "data DPL 3, CPL 0, RPL 0: max(CPL,RPL) <= DPL"),
    0x001B: ("ok",  "both",    "data DPL 3, RPL 3: max(0,3) <= 3"),
    0x0020: ("#0B", "both",    "not present -> #NP, error code = selector"),
    0x0028: ("#0D", "both",    "execute-only code is not a legal DS -> #GP"),
    0x0030: ("ok",  "both",    "expand-down data is a legal DS"),
    0x0038: ("ok",  "both",    "byte-granular limit 0FFFh loads as-is"),
    0x0080: ("#0D", "both",    "GDT index past the table limit -> #GP, error code = selector"),
    0x0088: ("#0D", "both",    "GDT index past the table limit -> #GP"),
    # LDTR is null out of reset. Both references keep a reset LDTR cache with
    # the present bit set, read a descriptor from linear 0 and load whatever
    # is there — different garbage each, since it is IVT contents. We fault.
    0x0084: ("#0D", "neither", "TI=1 with LDTR never loaded -> #GP (SDM 3A, segment descriptor tables)"),
    0x000C: ("#0D", "neither", "TI=1 with LDTR never loaded -> #GP"),
}
