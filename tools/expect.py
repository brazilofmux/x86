#!/usr/bin/env python3
"""expect.py — drive dos-monster by what is on its screen.

    tools/expect.py [-t SECONDS] [-v] SCRIPT -- dos-monster ARGS...

The emulator runs with X86_SCREEN pointing at a scratch file, which it
rewrites with the text screen four times a second. SCRIPT holds one step
per line: a regular expression, a TAB, and the keys to send once the
expression matches the last three non-blank lines of the screen (where a
prompt and the cursor are; a pattern starting with * is searched in the
whole screen, for dialogs drawn mid-screen). Keys use Python escapes: \\r is Enter, \\x1b+
puts the next diskette of an -fda sequence in. "delay SECONDS" pauses.
Blank lines and # comments are ignored. After each send the next step
waits for the screen to change, so a prompt still showing is not taken
twice. Exits 0 when every step matched (the emulator is stopped a
second after the last send), 1 on a timeout, printing the screen for
the post-mortem.
-v prints each step as it matches.
"""
import os, re, subprocess, sys, tempfile, time

def screen(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return ""

def tail(text, n=3):
    lines = [l.rstrip() for l in text.splitlines() if l.strip()]
    return "\n".join(lines[-n:])

def main():
    args = sys.argv[1:]
    timeout, verbose = 600.0, False
    while args and args[0].startswith("-") and args[0] != "--":
        if args[0] == "-t": timeout = float(args[1]); args = args[2:]
        elif args[0] == "-v": verbose = True; args = args[1:]
        else: break
    script, sep, cmd = args[0], args[1], args[2:]
    assert sep == "--"
    steps = []
    for line in open(script, encoding="utf-8"):
        line = line.rstrip("\n")
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        if line.startswith("delay "):
            steps.append(("delay", float(line.split()[1]), False))
            continue
        pat, _, keys = line.partition("\t")
        whole = pat.startswith("*")                 # *PATTERN: anywhere on the screen
        if whole: pat = pat[1:]
        steps.append((re.compile(pat), keys.encode().decode("unicode_escape").encode("latin-1"), whole))
    scr = os.path.join(tempfile.mkdtemp(), "screen.txt")
    env = dict(os.environ, X86_SCREEN=scr)
    p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, env=env)
    deadline = time.time() + timeout
    last_sent = None                            # the screen as it was when keys last went in
    for pat, keys, whole in steps:
        if pat == "delay":
            time.sleep(keys); continue
        while True:
            s = screen(scr)
            if s and s != last_sent and pat.search(s if whole else tail(s)):
                break
            if time.time() > deadline or p.poll() is not None:
                sys.stderr.write("expect: no %r; the screen:\n%s\n" % (pat.pattern, s))
                p.kill()
                return 1
            time.sleep(0.25)
        if verbose:
            sys.stderr.write("expect: %s\n" % pat.pattern)
        time.sleep(0.3)                          # the prompt is up; let it reach its input call
        last_sent = screen(scr)
        p.stdin.write(keys); p.stdin.flush()
    time.sleep(1.0)                              # what the last keys started, onto the screen
    p.terminate()
    try:
        p.wait(timeout=5)
    except subprocess.TimeoutExpired:
        p.kill()
    sys.stdout.write(screen(scr))
    return 0

if __name__ == "__main__":
    sys.exit(main())
