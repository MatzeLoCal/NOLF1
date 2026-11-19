#!/usr/bin/env python3
# Linux port: macOS is case-insensitive, Linux is not. For every #include "x"
# that only resolves case-insensitively, create a symlink with the exact
# spelling next to the real file (per directory, so each DLL keeps its own
# stdafx.h). Idempotent; prints what it created.
import os, re, sys
R = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
build = open(os.path.join(R, 'engine/build_linux.sh')).read()
incdirs = [p.replace('$R', R) for p in re.findall(r'-(?:I|iquote)(\$R/[^\s"\\]+)', build)]
inc_re = re.compile(r'^\s*#\s*include\s*["<]([^">]+)[">]', re.M)
created = []
def ci_resolve(base, rel):
    """component-wise case-insensitive resolve; returns list of (dir, wanted, actual)"""
    parts = [p for p in re.split(r'[\\/]', rel) if p not in ('', '.')]
    cur = base; fixes = []
    for p in parts:
        if p == '..':
            cur = os.path.dirname(cur); continue
        cand = os.path.join(cur, p)
        if os.path.lexists(cand):
            cur = cand; continue
        try: names = os.listdir(cur)
        except OSError: return None
        m = [n for n in names if n.lower() == p.lower()]
        if not m: return None
        fixes.append((cur, p, m[0])); cur = os.path.join(cur, m[0])
    return fixes
for root, dirs, files in os.walk(R):
    if '/.git' in root or '/build-linux' in root: continue
    for f in files:
        if not f.lower().endswith(('.c', '.cpp', '.h', '.hpp', '.inl', '.y', '.l')): continue
        path = os.path.join(root, f)
        try: txt = open(path, encoding='latin1').read()
        except OSError: continue
        for inc in inc_re.findall(txt):
            if '\\' in inc: continue          # handled by compat/include-shims
            bases = [root] + incdirs
            if any(os.path.exists(os.path.join(b, inc)) for b in bases): continue
            for b in bases:
                fx = ci_resolve(b, inc)
                if fx:
                    for d, want, actual in fx:
                        link = os.path.join(d, want)
                        if not os.path.lexists(link):
                            os.symlink(actual, link); created.append(os.path.relpath(link, R))
                    break
# broken symlinks (e.g. compat/include-shims) whose target differs only in case
for root, dirs, files in os.walk(R):
    if '/.git' in root or '/build-linux' in root: continue
    for f in files + dirs:
        l = os.path.join(root, f)
        if os.path.islink(l) and not os.path.exists(l):
            tgt = os.path.normpath(os.path.join(root, os.readlink(l)))
            fx = ci_resolve('/', tgt)
            if fx:
                for d, want, actual in fx:
                    link = os.path.join(d, want)
                    if not os.path.lexists(link):
                        os.symlink(actual, link); created.append(os.path.relpath(link, R))
for c in sorted(set(created)): print(c)
print(len(set(created)), 'links', file=sys.stderr)
