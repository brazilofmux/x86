#!/usr/bin/env python3
"""bochsrc for this host. The checked-in bochsrc is Homebrew's (macOS):
its ROM paths and the compiled-in nogui display. Ubuntu's bochs 2.7 has
neither: its BIOS-bochs-latest jumps into its own date string on this
floppy-only machine (BIOS-bochs-legacy boots it), and there is no nogui
plugin, so the display is rfb with no wait for a client. The debugger is
built in on both."""
import glob, os, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
UBUNTU_ROM = "/usr/share/bochs/BIOS-bochs-legacy"
UBUNTU_VGA = "/usr/share/bochs/VGABIOS-lgpl-latest"

def _has_nogui():
    return any(glob.glob(d + "/libbx_nogui*") for d in glob.glob("/usr/lib/*/bochs/plugins") + ["/usr/lib/bochs/plugins"]) \
        or os.path.exists("/opt/homebrew/share/bochs")

def write(floppy, extra=()):
    """A temporary bochsrc: the checked-in one, booting `floppy`, fixed up
    for this host, plus `extra` lines. Returns its path."""
    out = []
    for line in open(os.path.join(HERE, "bochsrc")):
        key = line.split(":", 1)[0].strip()
        if key in ("romimage", "vgaromimage"):
            path = line.split("file=", 1)[1].split(",")[0].strip()
            if not os.path.exists(path):
                line = "%s: file=%s\n" % (key, UBUNTU_ROM if key == "romimage" else UBUNTU_VGA)
        elif key == "floppya":
            line = "floppya: 1_44=%s, status=inserted\n" % floppy
        elif key == "display_library" and not _has_nogui():
            line = 'display_library: rfb, options="timeout=0"\n'
        out.append(line)
    if not _has_nogui():
        out.append("speaker: enabled=0\n")
    out += [l + "\n" for l in extra]
    fd, path = tempfile.mkstemp(prefix="bochsrc-", suffix=".txt")
    with os.fdopen(fd, "w") as f:
        f.writelines(out)
    return path

_dbg = None
def debugger_args():
    """["-debugger"] where the build needs it to enter its debugger
    (Homebrew's); Ubuntu's is always in the debugger and rejects the flag."""
    global _dbg
    if _dbg is None:
        h = __import__("subprocess").run(["bochs", "--help"], capture_output=True, text=True)
        _dbg = ["-debugger"] if "-debugger" in (h.stdout + h.stderr).split("Usage:")[-1] else []
    return _dbg
