"""What the architecture requires, independent of what any emulator does.

Keyed by (CPL, target, selector, aux). aux is the IRET frame's SS,
and zero for everything else. Each entry is:

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
    (0, "ds", 0x0000, 0): ("ok",  "both",    "null into a non-stack register is allowed; the segment becomes unusable"),
    (0, "ds", 0x0003, 0): ("ok",  "both",    "null with RPL 3 is still null"),
    (0, "ds", 0x0008, 0): ("ok",  "both",    "readable code is a legal data segment"),
    (0, "ds", 0x0010, 0): ("ok",  "both",    "plain read/write data"),
    (0, "ds", 0x0018, 0): ("ok",  "both",    "data DPL 3, CPL 0, RPL 0: max(CPL,RPL) <= DPL"),
    (0, "ds", 0x001B, 0): ("ok",  "both",    "data DPL 3, RPL 3: max(0,3) <= 3"),
    (0, "ds", 0x0020, 0): ("#0B", "both",    "not present -> #NP, error code = selector"),
    (0, "ds", 0x0028, 0): ("#0D", "both",    "execute-only code is not readable -> #GP"),
    (0, "ds", 0x0030, 0): ("ok",  "both",    "expand-down data is a legal DS"),
    (0, "ds", 0x0038, 0): ("ok",  "both",    "byte-granular limit 0FFFh loads as-is"),
    (0, "ds", 0x0040, 0): ("ok",  "both",    "read-only data is a legal DS; only SS demands writable"),
    (0, "ds", 0x00F0, 0): ("#0D", "both",    "GDT index past the table limit -> #GP, error code = selector"),
    (0, "ds", 0x00F8, 0): ("#0D", "both",    "GDT index past the table limit -> #GP"),
    (0, "ds", 0x0084, 0): ("#0D", "neither", "TI=1 with LDTR never loaded -> #GP (LDTR is null out of reset)"),
    (0, "ds", 0x008C, 0): ("#0D", "neither", "TI=1 with LDTR never loaded -> #GP"),

    # ---- SS: stricter than DS on every axis, and its own fault vector
    (0, "ss", 0x0010, 0): ("ok",  "both",    "writable data at DPL = RPL = CPL is the only thing SS accepts"),
    (0, "ss", 0x0000, 0): ("#0D", "both",    "null into SS is #GP(0), unlike DS"),
    (0, "ss", 0x0020, 0): ("#0C", "both",    "a not-present stack segment is #SS, not #NP"),
    (0, "ss", 0x0028, 0): ("#0D", "both",    "code is never a legal SS"),
    (0, "ss", 0x0040, 0): ("#0D", "both",    "SS must be writable; read-only data is legal for DS but not SS"),
    (0, "ss", 0x0018, 0): ("#0D", "both",    "SS requires DPL = CPL; DPL 3 at CPL 0 is #GP even though DS takes it"),
    (0, "ss", 0x001B, 0): ("#0D", "both",    "SS requires RPL = CPL; error code masks the low three bits (0018, not 001B)"),

    # ---- far JMP/CALL at CPL 0. The selector is in the instruction, not AX.
    (0, "jmpf", 0x0008, 0): ("ok",  "both", "plain non-conforming code at the same privilege"),
    (0, "jmpf", 0x0048, 0): ("ok",  "both", "conforming code is reachable from CPL 0"),
    (0, "jmpf", 0x0050, 0): ("#0B", "both", "code segment not present -> #NP"),
    (0, "jmpf", 0x0010, 0): ("#0D", "both", "a data segment is not a transfer target -> #GP"),
    (0, "jmpf", 0x0000, 0): ("#0D", "both", "null selector as a transfer target -> #GP(0)"),
    (0, "jmpf", 0x00F0, 0): ("#0D", "both", "index past the GDT limit -> #GP"),
    (0, "jmpf", 0x0058, 0): ("ok",  "both", "JMP may go through a call gate; CS becomes the gate's target"),
    (0, "jmpf", 0x0060, 0): ("#0B", "both", "gate descriptor not present -> #NP, not #GP"),
    (0, "jmpf", 0x0068, 0): ("#0D", "both", "gate whose target selector is data -> #GP"),
    (0, "callf", 0x0008, 0): ("ok",  "both", "plain code; pushes a far return address"),
    (0, "callf", 0x0058, 0): ("ok",  "both", "through a call gate, CS becomes the gate's target"),
    (0, "callf", 0x0050, 0): ("#0B", "both", "code segment not present -> #NP"),
    (0, "callf", 0x0010, 0): ("#0D", "both", "data segment as a call target -> #GP"),
    (0, "callf", 0x0068, 0): ("#0D", "both", "gate whose target is data -> #GP, error code is the TARGET (0010), not the gate"),

    # ---- once LLDT has run, TI=1 selectors are real. Everything above this
    #      point in the table still ran with LDTR unloaded.
    (0, "lldt", 0x0088, 0): ("ok",  "both", "load LDTR from a GDT descriptor of type 2 (available LDT)"),
    (0, "ds", 0x0004, 0): ("ok",  "both", "TI=1 index 0 is LDT entry 0, not null: only TI=0 index 0 is the null selector"),
    (0, "ds", 0x000C, 0): ("ok",  "both", "LDT read/write data"),
    (0, "ds", 0x0014, 0): ("#0B", "both", "LDT entry not present -> #NP, error code = selector (TI bit included)"),
    (0, "ds", 0x001C, 0): ("ok",  "both", "LDT readable code is a legal DS"),
    (0, "ds", 0x00FC, 0): ("#0D", "both", "index past the LDT limit -> #GP"),
    (0, "jmpf", 0x001C, 0): ("ok",  "both", "a far jump may target a code segment in the LDT"),

    # ---- IRET. Whether SS:ESP come off the stack is decided by CS.RPL, so
    #      the returned SS:ESP is the evidence of which kind of return it was.
    (0, "iret", 0x0008, 0):      ("ok",  "both", "same privilege: pops EIP, CS, EFLAGS and nothing else"),
    (0, "iret", 0x0048, 0):      ("ok",  "both", "conforming code at RPL 0 is still a same-privilege return"),
    (0, "iret", 0x0073, 0x001B): ("ok",  "both", "CS.RPL 3 > CPL: also pops ESP and SS, landing on the ring 3 stack"),
    (0, "iret", 0x0010, 0):      ("#0D", "both", "a data segment as the return CS -> #GP"),
    (0, "iret", 0x0050, 0):      ("#0B", "both", "return CS not present -> #NP"),
    (0, "iret", 0x0000, 0):      ("#0D", "both", "null return CS -> #GP(0)"),
    (0, "iret", 0x0073, 0x0010): ("#0D", "both", "SS.RPL must match CS.RPL; the error code names the SS (0010), not the CS"),

    # ---- the same instructions at CPL 3. The CS that comes back says whether
    #      privilege changed: its RPL is the new CPL.
    (3, "ds", 0x0010, 0): ("#0D", "both", "DPL 0 data is unreachable from ring 3 -> #GP"),
    (3, "ds", 0x0018, 0): ("ok",  "both", "DPL 3 data, RPL 0: max(CPL,RPL) = 3 <= DPL"),
    (3, "ds", 0x001B, 0): ("ok",  "both", "DPL 3 data, RPL 3"),
    (3, "ds", 0x0008, 0): ("#0D", "both", "DPL 0 code is unreachable from ring 3"),
    (3, "jmpf", 0x0008, 0): ("#0D", "both", "non-conforming code at DPL 0 is not reachable from CPL 3"),
    (3, "jmpf", 0x0048, 0): ("ok",  "both", "conforming code is reachable, and CPL stays 3 (CS comes back 004B)"),
    (3, "callf", 0x0058, 0): ("#0D", "both", "a call gate needs DPL >= CPL; a DPL 0 gate is unusable from ring 3"),
    (3, "callf", 0x0080, 0): ("ok",  "both", "DPL 3 gate: the way in. CPL becomes 0 and the stack switches via the TSS"),
    (3, "jmpf", 0x0070, 0): ("ok",  "both", "DPL 3 code from ring 3 stays at ring 3 (CS comes back 0073)"),

    # ---- ES: same rules as DS, spot-checked
    (0, "es", 0x0020, 0): ("#0B", "both",    "not present -> #NP, as for DS"),
    (0, "es", 0x0010, 0): ("ok",  "both",    "plain data"),

    # ---- privilege. Ring 3 runs with IOPL 0 and a TSS with no I/O bitmap.
    (0, "cli", 0, 0):    ("ok",  "both", "CPL 0 <= IOPL: CLI is allowed"),
    (0, "in", 0, 0):     ("ok",  "both", "CPL 0 <= IOPL: port I/O needs no bitmap"),
    (3, "cli", 0, 0):    ("#0D", "both", "CLI with CPL > IOPL -> #GP(0)"),
    (3, "sti", 0, 0):    ("#0D", "both", "STI with CPL > IOPL -> #GP(0)"),
    (3, "hlt", 0, 0):    ("#0D", "both", "HLT is a CPL 0 instruction -> #GP(0)"),
    (3, "in", 0, 0):     ("#0D", "both", "CPL > IOPL consults the TSS I/O bitmap; port DA7Ah's bit lies past the TSS limit -> #GP(0)"),
    (3, "out", 0, 0):    ("#0D", "both", "likewise for OUT"),
    (3, "in imm", 0, 0): ("ok",  "both", "measured: the I/O-map base word is 0, so the bitmap starts INSIDE the TSS; port 60h's bit is at offset 0Ch (ESP1, zero) -> allowed. A zeroed TSS is not 'no bitmap'"),
    (3, "clts", 0, 0):   ("#0D", "both", "CLTS is CPL 0 only"),
    (3, "lmsw", 0, 0):   ("#0D", "both", "LMSW is CPL 0 only"),
    (3, "mov cr", 0, 0): ("#0D", "both", "MOV to a control register is CPL 0 only"),

    # ---- RETF: IRET's rules for CS and SS, without EFLAGS
    (0, "retf", 0x0008, 0):      ("ok",  "both", "same privilege: pops EIP and CS"),
    (0, "retf", 0x0048, 0):      ("ok",  "both", "conforming code at RPL 0 is a same-privilege return"),
    (0, "retf", 0x0073, 0x001B): ("ok",  "both", "CS.RPL 3 > CPL: also pops ESP and SS"),
    (0, "retf", 0x0010, 0):      ("#0D", "both", "a data segment as the return CS -> #GP"),
    (0, "retf", 0x0050, 0):      ("#0B", "both", "return CS not present -> #NP"),
    (0, "retf", 0x0073, 0x0010): ("#0D", "both", "SS.RPL must match CS.RPL; error code names the SS"),
    (3, "retf", 0x0008, 0):      ("#0D", "both", "a return may not go inward: RPL 0 < CPL 3 -> #GP, error code = the CS"),
}
