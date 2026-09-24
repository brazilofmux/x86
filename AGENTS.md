# Repository Guidelines — x86 DOS Monster

See CLAUDE.md for architecture, scope, and the things we are explicitly
NOT building (64-bit long mode, cycle counting, sound).

This is a play project with an absurd performance goal. Go overboard on
the DBT, special-case real applications, cheat for speed where the guest
can't tell, and ship ridiculous numbers.

## Structure

```
x86/
├── core/   # x86 ISA: decoder, interpreter, paging, state
├── dbt/    # The monster JIT (AArch64 backend)
├── dos/    # HLE DOS, MZ loader, DPMI host, host file mapping
├── pc/     # BIOS, video/VGA, keyboard, timer, disks, CMOS, mouse
├── tools/  # oracles, fuzzers, expect.py, the native BIOS routine's source
└── tests/  # DOS programs, boot tests (tests/boot/), SST runners
```

Generated artifacts (`*.o`, `dos-monster`, `*.log`) are not committed.
DOS software stays local — bring your own copies (`disks/`, git-ignored).
