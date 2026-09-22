# Repository Guidelines — x86 DOS Monster

See CLAUDE.md for architecture, scope, and the things we are explicitly
NOT building (64-bit mode, V86, paging, cycle counting, sound).

This is a play project with an absurd performance goal. Go overboard on
the DBT, special-case real applications, cheat for speed where the guest
can't tell, and ship ridiculous numbers.

## Structure

```
x86/
├── core/   # x86 ISA: decoder, interpreter, state
├── dbt/    # The monster JIT (a64 + x64 backends)
├── dos/    # HLE DOS, MZ loader, DPMI host, host file mapping
├── pc/     # Video, keyboard, timer, BIOS personality
├── tools/
├── tests/
└── docs/
```

Generated artifacts (`*.o`, `dos-monster`, `*.log`) are not committed.
DOS software stays local — bring your own copies.
