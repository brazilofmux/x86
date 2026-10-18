#!/usr/bin/env python3
"""ntthreads.py — the processes and threads of a Windows XP (5.1, x86,
non-PAE) guest from a dos-monster memory dump (X86_MEMDUMP=FILE writes
FILE and FILE.cr3 at exit):

    tools/ntthreads.py FILE [-u]

For each thread: its state, wait reason, priority and the kernel return
addresses found on its stack, named by loaded module (+offset); -u adds
the user-mode start address. Offsets are XP's (RTM through SP3).
"""
import struct, sys

ST = ["Init", "Ready", "Running", "Standby", "Term", "Wait", "Trans", "DefReady"]
WR = ["Executive", "FreePage", "PageIn", "PoolAlloc", "DelayExec", "Suspended", "UserRequest", "WrExecutive",
      "WrFreePage", "WrPageIn", "WrPoolAlloc", "WrDelayExec", "WrSuspended", "WrUserRequest", "WrEventPair",
      "WrQueue", "WrLpcReceive", "WrLpcReply", "WrVirtualMemory", "WrPageOut", "WrRendezvous", "Spare2", "Spare3",
      "Spare4", "Spare5", "Spare6", "WrKernel", "WrResource", "WrPushLock", "WrMutex", "WrQuantumEnd", "WrDispatchInt",
      "WrPreempted", "WrYieldExecution"]

mem = open(sys.argv[1], "rb").read()
cr3 = int(open(sys.argv[1] + ".cr3").read(), 16)

def phys32(p): return struct.unpack_from("<I", mem, p)[0] if p + 4 <= len(mem) else None
def v2p(v, cr3=cr3):
    pde = phys32((cr3 & ~0xFFF) | ((v >> 20) & 0xFFC))
    if pde is None or not pde & 1: return None
    if pde & 0x80: return (pde & 0xFFC00000) | (v & 0x3FFFFF)
    pte = phys32((pde & ~0xFFF) | ((v >> 10) & 0xFFC))
    if pte is None or not pte & 1: return None
    return (pte & ~0xFFF) | (v & 0xFFF)
def rd(v, n=4, cr3=cr3):
    out = b""
    while n:
        p = v2p(v, cr3)
        k = min(n, 0x1000 - (v & 0xFFF))
        if p is None or p + k > len(mem): return None
        out += mem[p:p + k]; v += k; n -= k
    return out
def u32(v, cr3=cr3):
    b = rd(v, 4, cr3); return struct.unpack("<I", b)[0] if b else None
def u8(v):
    b = rd(v, 1); return b[0] if b else None
def ustr(v):    # UNICODE_STRING
    b = rd(v, 8)
    if not b: return "?"
    ln, _, buf = struct.unpack("<HHI", b)
    s = rd(buf, ln)
    return s.decode("utf-16le", "replace") if s else "?"

# loaded modules: PsLoadedModuleList, found from the KPCR's KdVersionBlock (KPCR+0x34)
mods = []
kdv = u32(0xFFDFF034)
if kdv:
    psl = u32(kdv + 0x18)          # DBGKD_GET_VERSION64.PsLoadedModuleList
    if psl:
        e = u32(psl)
        for _ in range(400):
            if not e or e == psl: break
            base, size = u32(e + 0x18), u32(e + 0x20)
            mods.append((base, size, ustr(e + 0x2C)))
            e = u32(e)
def name(a):
    for b, s, n in mods:
        if b <= a < b + s: return "%s+%X" % (n, a - b)
    return None

def objname(o):
    t = u8(o)                          # DISPATCHER_HEADER.Type
    kinds = {0: "Event(N)", 1: "Event(S)", 2: "Mutant", 3: "Process", 4: "Queue", 5: "Semaphore", 6: "Thread", 8: "Timer(N)", 9: "Timer(S)"}
    s = "%s %08X" % (kinds.get(t, "type%s" % t), o)
    hdr = o - 0x18
    noff = u8(hdr + 0x0C)
    if noff:
        nm = ustr(hdr - noff + 4)
        if nm and nm != "?": s += " '%s'" % nm
    if t == 6:                         # a thread: whose
        s += " (tid %s)" % u32(o + 0x1F0)
    if t == 3:
        s += " (%s)" % (rd(o + 0x174, 16) or b"?").split(b"\0")[0].decode("latin-1")
    return s

cur = u32(0xFFDFF124)
proc0 = u32(cur + 0x44)
print("current thread %08X, process %08X, %d modules" % (cur, proc0, len(mods)))
# ActiveProcessLinks (EPROCESS+0x88): walk from the current process all the way round
procs = [proc0]
# PsActiveProcessHead: KDDEBUGGER_DATA64 (DBGKD_GET_VERSION64.DebuggerDataList, +0x20, a list entry) +0x50
dl = u32(kdv + 0x20) if kdv else None
head = u32(u32(dl) + 0x50) if dl and u32(dl) else None
if head:
    e, n = u32(head), 0
    while e and e != head and n < 300:
        procs.append(e - 0x88); e = u32(e); n += 1
for p in procs:
    nm = (rd(p + 0x174, 16) or b"?").split(b"\0")[0].decode("latin-1")
    pid = u32(p + 0x84)
    if pid is None: continue
    pcr3 = u32(p + 0x18)    # DirectoryTableBase
    print("process %08X pid %d %s" % (p, pid, nm))
    umods = []
    peb = u32(p + 0x1B0)
    if peb and pcr3:
        ldr = u32(peb + 0x0C, pcr3)
        if ldr:
            hd = ldr + 0x0C; e = u32(hd, pcr3); k = 0
            while e and e != hd and k < 300:
                b, sz = u32(e + 0x18, pcr3), u32(e + 0x20, pcr3)
                bn = rd(e + 0x2C, 8, pcr3)
                nm2 = "?"
                if bn:
                    ln, _, buf = struct.unpack("<HHI", bn); x = rd(buf, ln, pcr3); nm2 = x.decode("utf-16le", "replace") if x else "?"
                if b: umods.append((b, sz or 0, nm2))
                e = u32(e, pcr3); k += 1
    def uname(a):
        for b, sz, n in umods:
            if b <= a < b + sz: return "%s+%X" % (n, a - b)
        return None
    head = p + 0x50
    t = u32(head); n = 0
    while t and t != head and n < 300:
        th = t - 0x1B0; n += 1
        st, pri, wr = u8(th + 0x2D), u8(th + 0x33), u8(th + 0x5B)
        tid = u32(th + 0x1F0)
        ks, isk = u32(th + 0x28), u32(th + 0x18)
        start = u32(th + 0x228)
        line = "  tid %4s %-8s %-14s pri %2s" % (tid, ST[st] if st is not None and st < len(ST) else st,
                                                  (WR[wr] if wr is not None and wr < len(WR) else str(wr)) if st == 5 else "", pri)
        if "-u" in sys.argv and start: line += " start %08X" % start
        if st == 5:                 # what it waits on: WaitBlockList -> objects, by type and name
            wb = u32(th + 0x5C); objs = []; k = 0
            while wb and k < 4:
                o = u32(wb + 0x0C)
                if o: objs.append(objname(o))
                nx = u32(wb + 0x14)  # NextWaitBlock
                k += 1
                if not nx or nx == u32(th + 0x5C): break
                wb = nx
            if objs: line += "  on " + ", ".join(objs)
        print(line + (" <- current" if th == cur else ""))
        if ks and isk and isk > ks and isk - ks < 0x6000:
            stack = rd(ks, isk - ks, pcr3 or cr3) or rd(ks, isk - ks)
            if stack:
                ras = []
                for i in range(0, len(stack) - 3, 4):
                    v = struct.unpack_from("<I", stack, i)[0]
                    nmv = name(v) if v >= 0x80000000 else None
                    if nmv and not nmv.endswith("+0"): ras.append(nmv)
                    if len(ras) >= 14: break
                print("      " + " ".join(ras))
        tf = u32(th + 0x134)        # KTHREAD.TrapFrame: the user-mode side
        if "-u" in sys.argv and tf and pcr3:
            uesp = u32(tf + 0x74); ueip = u32(tf + 0x68)
            if uesp and uesp < 0x80000000:
                us = rd(uesp, 0x800, pcr3)
                ras = [uname(ueip) or "%08X" % ueip] if ueip else []
                if us:
                    for i in range(0, len(us) - 3, 4):
                        v = struct.unpack_from("<I", us, i)[0]
                        nv = uname(v) if v < 0x80000000 else None
                        if nv: ras.append(nv)
                        if len(ras) >= 16: break
                print("      user: " + " ".join(ras))
        t = u32(t)
