#!/usr/bin/env python3
"""
gen_skeleton.py -- generate the engine skeleton from LT2's own SDK headers.

WHY GENERATED RATHER THAN HAND-WRITTEN
--------------------------------------
The LT2 interfaces declare ~265 pure virtual methods across 19 headers. Hand
writing that many stubs invites two failures: a typo'd signature that silently
becomes an overload instead of an override, and drift when a header changes.
Generating from the headers makes both impossible -- and the generator can be
re-run.

⚠️ THE DEFAULT RETURN IS A DELIBERATE CHOICE: LT_ERROR, NOT LT_OK.
------------------------------------------------------------------
A skeleton could return LT_OK with zeroed outputs. The game would then get much
further and we would see more of its call sequence -- which is tempting.

It is also a lie. "Succeeded" when nothing happened is exactly the silent
failure that costs weeks: the game proceeds on data it never received, and the
eventual symptom appears far from the cause.

So every stub returns LT_ERROR and announces itself. The game will stop at the
first call it genuinely needs, and THAT is the most useful thing a skeleton can
tell us -- it names the next thing to implement, in dependency order, without
guesswork.

⚠️ NOTHING IS SILENTLY SKIPPED. Any declaration this script cannot parse is
reported and counted; the generated file will fail to compile rather than
quietly omit a method.
"""

import re, sys, os

SDK = os.path.join(os.path.dirname(__file__), '..', '..', 'LT2', 'sdk', 'inc')
OUT = os.path.dirname(__file__)

# Interfaces to generate concrete stubs for.
HEADERS = [
    'iltcsbase.h', 'iltclient.h', 'iltserver.h', 'iltcommon.h',
    'iltmodel.h', 'iltphysics.h', 'iltsoundmgr.h', 'ilttransform.h',
    'iltmath.h', 'iltmessage.h', 'iltcursor.h', 'iltcustomdraw.h',
    'iltdirectmusic.h', 'iltlightanim.h', 'iltprelight.h',
    'iltspritecontrol.h', 'ilttexmod.h', 'iltvideomgr.h',
]

# ⚠️ HAND-WRITTEN, NOT STUBBED. These are declared in the SDK headers WITHOUT
# `= 0`, so the interface vtable still needs an out-of-line body -- but a stub
# is the wrong body: they are the message pipe, and they are REAL, in
# engine/shared/ltmessage.cpp. One definition there serves both halves of the
# game, because ILTCSBase is the base ILTClient and ILTServer share.
#
# ⚠️ NOTHING IS SILENTLY SKIPPED HERE EITHER. A name in this set that the
# headers do not declare is reported and fails the run -- otherwise a renamed
# method would quietly go back to being a stub with nobody the wiser, which is
# the exact failure this generator exists to prevent.
HANDWRITTEN = {
    'ILTCSBase': {
        'WriteToMessageFloat', 'WriteToMessageByte', 'WriteToMessageWord',
        'WriteToMessageDWord', 'WriteToMessageString', 'WriteToMessageVector',
        'WriteToMessageCompVector', 'WriteToMessageCompPosition',
        'WriteToMessageRotation', 'WriteToMessageHString',
        'WriteToMessageHMessageWrite', 'WriteToMessageHMessageRead',
        'WriteToMessageFormattedHString', 'WriteToMessageObject',
        'WriteToLoadSaveMessageObject',
        'ReadFromMessageFloat', 'ReadFromMessageByte', 'ReadFromMessageWord',
        'ReadFromMessageDWord', 'ReadFromMessageVector',
        'ReadFromMessageCompVector', 'ReadFromMessageCompPosition',
        'ReadFromMessageRotation', 'ReadFromMessageObject',
        'ReadFromMessageHString', 'ReadFromLoadSaveMessageObject',
        'ReadFromMessageHMessageRead',
        'EndHMessageRead', 'EndHMessageWrite', 'ResetRead',
    },
    # ⚠️ ILTMath is embedded BY VALUE in ILTCSBase (`ILTMath m_MathLT`, and
    # GetMathLT returns its address), so the game's g_pMathLT IS this base
    # object and no subclass can stand in for it -- the base methods themselves
    # must be real. As stubs they left every AI rotation uninitialised, and a
    # garbage quaternion shears a model: the NPCs were flat discs.
    'ILTMath': {
        'GetRotationVectors', 'InterpolateRotation', 'SetupEuler',
        'SetupTransformationMatrix', 'SetupTranslationMatrix',
        'SetupRotationMatrix', 'SetupTranslationFromMatrix',
        'SetupRotationFromMatrix', 'SetupRotationAroundPoint',
        'AlignRotation', 'EulerRotateX', 'EulerRotateY', 'EulerRotateZ',
        'RotateAroundAxis', 'GetRotationVectorsFromMatrix',
    },
}

# Where each class's hand-written methods live, for the pointer comment the
# generated file leaves in their place.
HANDWRITTEN_FILE = {
    'ILTCSBase': 'engine/shared/ltmessage.cpp',
    'ILTMath':   'engine/kernel/math/ltmathlt.cpp',
}

# Return-type -> expression producing a defined value.
def default_for(rt):
    rt = rt.strip()
    if rt == 'void':
        return None
    if rt == 'LTRESULT':
        return 'LT_ERROR'
    if rt.endswith('*') or rt.endswith('&'):
        return '0'
    if rt in ('bool',):
        return 'false'
    if rt in ('LTBOOL',):
        return 'LTFALSE'
    if rt in ('float', 'double', 'LTFLOAT', 'LTDFLOAT'):
        return '0.0f'
    # Handles, ints, enums and anything else: a zero of that type.
    return '(%s)0' % rt

# ⚠️ Captures the optional base: ILTClient : public ILTCSBase. A stub that
# overrides only its OWN pure virtuals stays abstract, because the base's are
# still unimplemented. The compiler catches this, but only at use.
CLASS_RE = re.compile(
    r'\bclass\s+(?P<name>I[A-Za-z0-9_]+)\b\s*'
    r'(?::\s*public\s+(?P<base>I[A-Za-z0-9_]+))?[^;{]*\{', re.M)

# virtual <ret> <name> ( <args> ) [const] = 0 ;
#
# ⚠️ The (?P<ptr>[*&]*) group is load-bearing. Declarations write the pointer on
# EITHER side of the space -- `void *GetData()` as well as `void* GetData()`.
# Without it the first form silently fails to parse, which is precisely what the
# unparsed-declaration check caught.
# Matches BOTH forms that need a definition:
#     virtual R f(a) = 0;      pure
#     virtual R f(a);          declared, no inline body
# and deliberately NOT `virtual R f(a) { ... }`, which already has one.
PURE_RE = re.compile(
    r'virtual\s+(?P<ret>[A-Za-z_][A-Za-z0-9_:<>\s\*&]*?)\s+'
    r'(?P<ptr>[*&]*)\s*'
    r'(?P<name>[A-Za-z_][A-Za-z0-9_]*)\s*'
    r'\((?P<args>[^;{]*?)\)\s*(?P<cv>const)?\s*(?:=\s*0\s*)?;', re.S)


def strip_comments(text):
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    text = re.sub(r'//[^\n]*', '', text)
    return text


def class_bodies(text):
    """Yield (classname, body) for each class, matching braces."""
    for m in CLASS_RE.finditer(text):
        name = m.group('name')
        base = m.group('base')
        i = m.end() - 1          # at the '{'
        depth = 0
        for j in range(i, len(text)):
            if text[j] == '{':
                depth += 1
            elif text[j] == '}':
                depth -= 1
                if depth == 0:
                    yield name, base, text[i + 1:j]
                    break


def param_names_stripped(args):
    """Arg list with parameter NAMES kept, whitespace NORMALISED.

    ⚠️ Normalising is not cosmetic. The same method is declared in both a base
    and a derived interface with different tabbing, so an un-normalised string
    makes the dedup key miss and the member gets emitted twice -- which is a
    redeclaration error, not a silent problem. Multi-line declarations also
    collapse to one line here, which the emitted C++ requires.
    """
    a = ' '.join(args.split())
    return '' if a in ('', 'void') else a




# ⚠️ Typedef aliases must be resolved before comparing signatures.
#
#   ILTCSBase::SetModelAnimation(HOBJECT,    HMODELANIM)
#   ILTClient::SetModelAnimation(HLOCALOBJ,  uint32)
#
# HOBJECT and HLOCALOBJ are BOTH `LTObject*`, and HMODELANIM IS `uint32`. So
# those are one signature to the compiler and two to naive text comparison --
# ILTClient re-declares a method it already inherits, using different spellings.
# Emitting both is a redeclaration error, so the key canonicalises through the
# SDK's own typedefs rather than a hand-written list.
ALIASES = {}

def load_aliases():
    for hdr in ('ltbasetypes.h', 'ltbasedefs.h'):
        path = os.path.join(SDK, hdr)
        if not os.path.exists(path):
            continue
        txt = strip_comments(open(path, errors='surrogateescape').read())
        for m in re.finditer(r'typedef\s+([A-Za-z_][A-Za-z0-9_\s\*]*?)\s*\*?\s*([A-Za-z_][A-Za-z0-9_]*)\s*;', txt):
            base, name = ' '.join(m.group(1).split()), m.group(2)
            star = '*' if '*' in m.group(0)[:m.group(0).rindex(name)] else ''
            ALIASES[name] = (base + star).strip()
    # Collapse chains (uint32 -> unsigned int, etc.)
    for _ in range(4):
        for k, v in list(ALIASES.items()):
            core = v.rstrip('*')
            if core in ALIASES and ALIASES[core] != v:
                ALIASES[k] = ALIASES[core] + ('*' if v.endswith('*') else '')


def canon_type(t):
    t = ' '.join(t.split())
    suffix = ''
    while t.endswith('*') or t.endswith('&'):
        suffix = t[-1] + suffix
        t = t[:-1].strip()
    t = re.sub(r'^(const|volatile)\s+', '', t).strip()
    return ALIASES.get(t, t) + suffix


def param_types_only(args):
    """Reduce an argument list to its TYPES, for use as a dedup key.

    ⚠️ Why this is needed: a method can be declared in BOTH a base and a derived
    interface (ILTCSBase and ILTClient both declare SetModelAnimation), and the
    two declarations may name their parameters differently. Keying on the full
    argument text then treats them as distinct and emits the member twice --
    a redeclaration error. Types are what actually determine the signature.
    """
    if not args:
        return ''
    out = []
    depth = 0
    cur = ''
    for ch in args + ',':
        if ch in '<(': depth += 1
        elif ch in '>)': depth -= 1
        if ch == ',' and depth == 0:
            t = ' '.join(cur.split())
            # Drop a trailing parameter name, and any default value.
            t = re.sub(r'=.*$', '', t).strip()
            t = re.sub(r'\b[A-Za-z_][A-Za-z0-9_]*$', '', t).strip()
            out.append(canon_type(t))
            cur = ''
        else:
            cur += ch
    return ','.join(out)



# ⚠️ THE INTERFACES ARE A HYBRID: virtuals AND function-pointer MEMBERS.
#
#   ILTCSBase / ILTCommon  -- pure C++ abstract classes (virtuals only)
#   ILTClient              -- 208 function-pointer members + a virtual base
#   ILTServer              -- 129 function-pointer members
#
# Declared as:   HSURFACE (*GetScreenSurface)();
#
# Overriding virtuals does NOTHING for these. Left unassigned they are NULL, and
# calling one jumps to address zero -- which is exactly how HookWindow() crashed
# (pc 0x0, via g_pLTClient->GetScreenSurface()). There is no diagnostic: the
# call never reaches a stub, so nothing reports.
#
# Each member therefore gets a real stub function and an explicit assignment.
FNPTR_RE = re.compile(
    r'(?P<ret>[A-Za-z_][A-Za-z0-9_:<>\s\*&]*?)\s*'
    r'\(\s*\*\s*(?P<name>[A-Za-z_][A-Za-z0-9_]*)\s*\)\s*'
    r'\((?P<args>[^;]*?)\)\s*;', re.S)



# ⚠️ SUB-INTERFACE DATA MEMBERS -- the third shape, and the one that keeps
# costing time.
#
#   ILTCSBase:  ILTCommon *m_pCommonLT;   ... 6 of them
#   ILTClient:  ILTVideoMgr *m_pVideoMgr; ILTTexMod *m_pTexMod;
#               ILTCursor *m_pCursorLT;   ILTDirectMusicMgr *m_pDirectMusicMgr;
#
# reached through INLINE accessors (`inline ILTCursor* Cursor() {return
# m_pCursorLT;}`), so no override and no stub is ever involved. Left NULL they
# are a null-pointer read with no [LTSTUB] line before it -- and the absence of
# that line is the only diagnostic there is.
#
# The host originally wired ILTCSBase's six by hand, which was correct and
# incomplete: ILTClient's four went unnoticed until the game got far enough to
# call g_pLTClient->Cursor()->LoadCursorBitmapResource() and segfaulted in
# CInterfaceMgr::InitCursor. Hand-maintaining this list means the next four are
# found the same way. Generating it means there is no next time.
#
# ⚠️ POINTER MEMBERS ONLY. `ILTMath m_MathLT;` is held BY VALUE and
# default-constructs; assigning to it is not the same thing at all.
DATAMEMBER_RE = re.compile(
    r'\b(?P<type>I[A-Za-z0-9_]+)\s*\*\s*(?P<name>m_[A-Za-z0-9_]+)\s*;')


ACCESS_RE = re.compile(r'^.*\b(?:public|protected|private)\s*:\s*')

def clean_ret(ret):
    """Strip an access label the regex may have swallowed, and validate.

    ⚠️ The return-type pattern permits ':' so qualified types (Foo::Bar) parse.
    That also lets it run backwards over a `public:` label, producing a return
    type of "public: LTRESULT" -- which compiles into nonsense. A count-based
    integrity check cannot catch it: the count is right, the content is not.
    Returns None if the result still does not look like a type.
    """
    r = ACCESS_RE.sub('', ' '.join(ret.split())).strip()
    if not r:
        return None
    # A lone ':' or a leftover keyword means the capture is still wrong.
    if re.search(r'(?<!:):(?!:)', r):
        return None
    if re.search(r'\b(public|protected|private|virtual|static)\b', r):
        return None
    return r

def emit_fnptr_stubs(registry_fn):
    """Return (stub_defs, wire_fns, count) for every function-pointer member."""
    defs, wires, total = [], [], 0
    for cname in sorted(registry_fn.keys()):
        members = registry_fn[cname]
        if not members:
            continue
        body = []
        for (ret, name, args) in members:
            # (validated in collection; see clean_ret)
            fn = 'LTFnStub_%s_%s' % (cname, name)
            dv = default_for(ret)
            b = 'LTSTUB("%s::%s");' % (cname, name)
            if dv is not None:
                b += ' return %s;' % dv
            defs.append('static %s %s(%s) { %s }' % (ret, fn, args, b))
            body.append('    p->%s = %s;' % (name, fn))
            total += 1
        wires.append('inline void LTSkel_WireFnPtrs_%s(%s* p)\n{\n%s\n}'
                     % (cname, cname, '\n'.join(body)))
    return defs, wires, total


def main():
    load_aliases()
    total, classes_done, unparsed = 0, 0, []
    data_total = [0]
    wiredata_defs = []
    used_headers = []
    registry = {}
    registry_fn = {}
    registry_data = {}
    handwritten_shadowed = {}
    out_lines = []
    decls = []

    # PASS 1 -- register every interface (needed before any base can be
    # resolved, since a derived class may appear in an earlier header).
    for h in HEADERS:
        path = os.path.join(SDK, h)
        if not os.path.exists(path):
            continue
        text = strip_comments(open(path, errors='surrogateescape').read())
        for cname, cbase, body in class_bodies(text):
            registry[cname] = (h, cbase, [
                (' '.join(x.group('ret').split()) + x.group('ptr'),
                 x.group('name'),
                 param_names_stripped(x.group('args')),
                 ' const' if x.group('cv') else '',
                 bool(re.search(r'=\s*0\s*;$', x.group(0).strip())))
                for x in PURE_RE.finditer(body)])

    # Sub-interface DATA members, collected the same way.
    for h in HEADERS:
        path = os.path.join(SDK, h)
        if not os.path.exists(path):
            continue
        text = strip_comments(open(path, errors='surrogateescape').read())
        for cname, cbase, body in class_bodies(text):
            registry_data[cname] = [(m.group('type'), m.group('name'))
                                    for m in DATAMEMBER_RE.finditer(body)]

    # Function-pointer members, collected the same way.
    for h in HEADERS:
        path = os.path.join(SDK, h)
        if not os.path.exists(path):
            continue
        text = strip_comments(open(path, errors='surrogateescape').read())
        for cname, cbase, body in class_bodies(text):
            found = list(FNPTR_RE.finditer(body))
            cleaned = []
            for m in found:
                r = clean_ret(m.group('ret'))
                if r is None:
                    unparsed.append('%s::%s fn-ptr %s -- unusable return type %r'
                                    % (h, cname, m.group('name'), m.group('ret')))
                    continue
                cleaned.append((r, m.group('name'), ' '.join(m.group('args').split())))
            registry_fn[cname] = cleaned
            # Nothing may be silently skipped: a missed pointer is a NULL call.
            rough = len(re.findall(r'\(\s*\*\s*[A-Za-z_]\w*\s*\)\s*\(', body))
            if rough != len(cleaned):
                unparsed.append('%s::%s fn-ptrs (%d found, %d parsed)'
                                % (h, cname, rough, len(found)))

    # PASS 2 -- emit.
    for h in HEADERS:
        path = os.path.join(SDK, h)
        if not os.path.exists(path):
            print('  MISSING HEADER: %s' % h)
            continue
        text = strip_comments(open(path, errors='surrogateescape').read())

        for cname, cbase, body in class_bodies(text):
            pures = list(PURE_RE.finditer(body))
            # NB: PASS 1 already registered every interface. Re-registering
            # here would clobber it with a different tuple shape.
            if not pures:
                continue

            # Anything that looks pure-virtual but did not parse must be seen.
            rough = len(re.findall(r'virtual[^;{]*(?:=\s*0\s*)?;', body, flags=re.S))
            if rough != len(pures):
                unparsed.append('%s::%s  (%d found, %d parsed)' %
                                (h, cname, rough, len(pures)))

            if h not in used_headers:
                used_headers.append(h)
            stub = 'LTStub_' + cname
            classes_done += 1
            out_lines.append('// %s -- from %s' % (stub, h))
            out_lines.append('class %s : public %s' % (stub, cname))
            out_lines.append('{')
            out_lines.append('public:')

            # Collect this interface's pure virtuals PLUS every one it
            # inherits, or the stub stays abstract.
            methods = []
            seen = set()
            walk, guard = cname, 0
            while walk and walk in registry and guard < 8:
                hdr, base, ms = registry[walk]
                for (ret, name, args, cv, _pure) in ms:
                    key = (name, param_types_only(args))
                    if key in seen:
                        continue        # derived override wins
                    seen.add(key)
                    methods.append((ret, name, args, cv, walk))
                walk = base
                guard += 1

            for (ret, name, args, cv, owner) in methods:
                # ⚠️ A HAND-WRITTEN METHOD MUST NOT BE OVERRIDDEN HERE EITHER.
                # Defining the real body on ILTCSBase and then re-declaring a
                # stub in LTStub_ILTServer/LTStub_ILTClient SHADOWS it: the
                # derived override wins the vtable slot, the real code is never
                # reached, and everything still links. That is exactly what
                # happened -- the message pipe was written, the census still
                # showed WriteToMessageByte as a stub, and only the census said
                # so. Skipping it here lets the base's definition be inherited,
                # which is the whole point of putting it on the base.
                if name in HANDWRITTEN.get(owner, ()):
                    handwritten_shadowed.setdefault(owner, set()).add(name)
                    continue
                dv = default_for(ret)
                body_txt = 'LTSTUB("%s::%s");' % (owner, name)
                if dv is not None:
                    body_txt += ' return %s;' % dv
                out_lines.append('    %s %s(%s)%s override { %s }'
                                 % (ret, name, args, cv, body_txt))
                total += 1

            # --- sub-interface data members, this class's and inherited ---
            #
            # Emitted as a MEMBER, not a free function: these are `protected`
            # on the interface, so only something inside the class hierarchy
            # can assign them.
            data = []
            walk, guard = cname, 0
            seen_data = set()
            while walk and walk in registry and guard < 8:
                for (mtype, mname) in registry_data.get(walk, []):
                    if mname in seen_data:
                        continue
                    seen_data.add(mname)
                    data.append((mtype, mname))
                walk = registry[walk][1]
                guard += 1

            if data:
                out_lines.append('')
                out_lines.append('    // Sub-interface DATA members. ⚠️ Not methods and not')
                out_lines.append('    // function pointers -- reached through INLINE accessors, so')
                out_lines.append('    // nothing here ever passes through a stub. Left NULL they are')
                out_lines.append('    // a null read with no [LTSTUB] line before it.')
                out_lines.append('    //')
                out_lines.append('    // Call this from the most-derived constructor, BEFORE any')
                out_lines.append('    // deliberate per-member override: the last assignment wins, and')
                out_lines.append('    // the host has reasons to prefer some variants (the CLIENT')
                out_lines.append('    // flavours of ILTPhysics and ILTSoundMgr, for instance).')
                out_lines.append('    //')
                out_lines.append('    // ⚠️ DECLARED here, DEFINED at the bottom of this header. The')
                out_lines.append('    // stub types it assigns are declared LATER in the file, and an')
                out_lines.append('    // inline body would not see them: a member function body is')
                out_lines.append('    // parsed in its own class\'s complete-class context, which')
                out_lines.append('    // delays lookup for THAT class\'s members only, not for types')
                out_lines.append('    // that appear further down.')
                out_lines.append('    void LTSkel_WireData();')

                b = ['inline void %s::LTSkel_WireData()' % stub, '{']
                for (mtype, mname) in data:
                    b.append('    { static LTStub_%s s; %s = &s; }' % (mtype, mname))
                b.append('}')
                wiredata_defs.append('\n'.join(b))
                data_total[0] += len(data)

            out_lines.append('};')
            out_lines.append('')
            decls.append((cname, stub))

    header = '''// ----------------------------------------------------------------------- //
//
// GENERATED FILE -- do not edit by hand.
//   Produced by engine/skeleton/gen_skeleton.py from LT2/sdk/inc/*.h
//   Re-run that script after any SDK header change.
//
// Concrete stand-ins for LT2's engine interfaces. Every method announces
// itself and returns LT_ERROR (see the script's header for why it is not
// LT_OK). The game will therefore stop at the first engine call it genuinely
// needs -- which names the next thing to implement.
//
// ----------------------------------------------------------------------- //

#ifndef __LT_ENGINE_SKELETON_GENERATED_H__
#define __LT_ENGINE_SKELETON_GENERATED_H__

// ⚠️ Include order mirrors the game's own Stdafx.h: windows.h first so
// _WINDOWS_ is defined, then the SDK. Without it ltbasedefs.h cannot see
// _MAX_PATH and mfcstub declares its own POINT/RECT.
#include "windows.h"
#include "lithtech.h"
#include "mfcstub.h"

// The interfaces these stubs derive from.
%s

#include "ltstub.h"

'''
    # Out-of-line definitions for every NON-PURE virtual, on the interface
    # itself. Without these the interface's own vtable is unresolved.
    impl = []
    handwritten_seen = {}
    for cname, (hdr, base, ms) in sorted(registry.items()):
        for (ret, name, args, cv, is_pure) in ms:
            if is_pure:
                continue
            if name in HANDWRITTEN.get(cname, ()):
                # Real implementation elsewhere -- emit a pointer to it, so the
                # generated file still ACCOUNTS for every method rather than
                # having a hole where one used to be.
                handwritten_seen.setdefault(cname, set()).add(name)
                impl.append('// %s::%s -- hand-written in %s'
                            % (cname, name, HANDWRITTEN_FILE.get(cname, '?')))
                continue
            dv = default_for(ret)
            b = 'LTSTUB("%s::%s");' % (cname, name)
            if dv is not None:
                b += ' return %s;' % dv
            impl.append('%s %s::%s(%s)%s { %s }' % (ret, cname, name, args, cv, b))

    # ⚠️ A NAME IN HANDWRITTEN THAT THE HEADERS NO LONGER DECLARE means the SDK
    # moved and the hand-written body is now dead code that nothing calls. Fail
    # loudly: a silently-unused definition is how the message pipe would come
    # back as a stub without anyone noticing.
    hw_missing = []
    for cname, names in HANDWRITTEN.items():
        seen = handwritten_seen.get(cname, set())
        for n in sorted(names - seen):
            hw_missing.append('%s::%s' % (cname, n))

    includes = '\n'.join('#include "%s"' % h for h in used_headers)
    with open(os.path.join(OUT, 'skeleton_generated.h'), 'w') as f:
        f.write(header % includes)
        f.write('\n'.join(out_lines))
        f.write('\n#endif // __LT_ENGINE_SKELETON_GENERATED_H__\n')

    with open(os.path.join(OUT, 'skeleton_generated.cpp'), 'w') as f:
        f.write('// GENERATED -- see gen_skeleton.py. Out-of-line definitions for\n'
                '// non-pure virtuals, which the INTERFACE vtables require.\n\n')
        f.write('#include "skeleton_generated.h"\n\n')
        f.write('\n'.join(impl))
        f.write('\n')

    # Out-of-line LTSkel_WireData bodies, after every stub class exists.
    if wiredata_defs:
        with open(os.path.join(OUT, 'skeleton_generated.h'), 'a') as f:
            f.write('\n// --- sub-interface data-member wiring ------------------------------\n')
            f.write('// Defined here, not inline in the classes: the stub types assigned below\n')
            f.write('// are declared further up but AFTER the classes that use them.\n')
            f.write('\n'.join(wiredata_defs))
            f.write('\n')

    fn_defs, fn_wires, fn_total = emit_fnptr_stubs(registry_fn)
    with open(os.path.join(OUT, 'skeleton_generated.h'), 'a') as f:
        f.write('\n// --- function-pointer members: stubs -------------------------------\n')
        f.write('\n'.join(fn_defs))
        f.write('\n\n// --- function-pointer members: wiring ------------------------------\n')
        f.write('\n\n'.join(fn_wires))
        f.write('\n')

    print('generated %d stub methods across %d interfaces' % (total, classes_done))
    print('  + %d function-pointer member stubs' % fn_total)
    n_shadow = sum(len(v) for v in handwritten_shadowed.values())
    print('  - %d stub-class overrides suppressed so the hand-written '
          'base definitions are inherited' % n_shadow)
    n_hw = sum(len(v) for v in handwritten_seen.values())
    print('  + %d out-of-line non-pure virtual definitions '
          '(%d of them hand-written elsewhere, not stubbed)'
          % (len(impl), n_hw))
    if hw_missing:
        print('\n⚠️  HANDWRITTEN NAMES THE HEADERS NO LONGER DECLARE:')
        for m in hw_missing:
            print('     ' + m)
        sys.exit(1)
    print('  + %d sub-interface data-member assignments' % data_total[0])
    if unparsed:
        print('\n⚠️  DECLARATIONS NOT PARSED (these would be silently missing):')
        for u in unparsed:
            print('     ' + u)
        return 1
    print('all pure virtuals parsed')
    return 0


sys.exit(main())
