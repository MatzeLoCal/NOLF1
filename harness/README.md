# The frame-contract harness

Built 2026-08-25, before there is anything to render — which is the point.
`PORT_PLAN.md` §4 and `Docs/PORTING_NOLF1_BACKGROUND.md` §3.1 both say the same
thing in capitals: **build the harness first, not after the renderer works.**

The NOLF 2 port's contract is four archived `c01s01` frames at **0.00 mean
absolute difference**. It caught regressions no play-test would have, and it is
the reason a GL→Metal conversion was survivable at all.

---

## What a frame contract actually is

Not "pixels". The shape is:

> a **deterministic artefact** of a known-good run, **archived**, **diffed
> automatically** on every change, where **any drift is a failure** until a
> human deliberately says otherwise.

NOLF 1 cannot draw anything yet, so there are no pixels. But it *does* produce a
deterministic artefact today: the **ordered sequence of engine calls and file
accesses**. That is a real contract with real teeth, and it covers the thing
that regressed silently during the file-system work (four `DebugOut` calls
quietly became four separate census entries).

So the harness has two halves, and only one of them is waiting:

| half | artefact | status |
|---|---|---|
| **run record** | ordered engine calls + file accesses | **live — 148 events** |
| **framediff** | rendered frames | **LIVE as of 2026-08-25** |

⚠️ **The pixel half is no longer waiting.** The port renders: NOLF's splash
screen at `reference/frames/splash_f120.ppm`, gated at **0.000 mean absolute
difference**. It was verified to fail on one channel of one pixel changed by 40
— reported as `max 40 at (0,0)` while the mean still reads 0.000, which is
precisely the defect a mean-only differ waves through.

**Reproducibility is what makes this a contract rather than a snapshot.** The
frame run uses a fixed timestep (`engine/kernel/time/`), advanced once per frame
by the host and by nothing else, at a fixed frame count. ⚠️ `LT_REALTIME=1`
exists for interactive runs and must never be set by the harness.

---

## Files

```
runrecord.sh            headless run recipe -> one run record
checkcontract.sh        THE GATE: run, diff against reference, non-zero on drift
framediff.py            pixel differ, with a real pass/fail threshold
selftest.sh             validates the INSTRUMENTS themselves
enginetests.sh          unit tests for engine subsystems, against real data
enginetest_strings.cpp    ClientRes + HSTRING, 20 checks
enginetest_pcx.cpp        PCX decoding, 17 checks
enginetest_2d.cpp         the 2D compositor, 23 checks (no game data needed)
reference/              the archived known-good record
reference/frames/         the archived known-good frame
```

## Using it

```bash
ClaudeCode/NOLF/engine/build_skeleton.sh     # build
ClaudeCode/NOLF/harness/selftest.sh          # are the INSTRUMENTS sound?
ClaudeCode/NOLF/harness/enginetests.sh       # is each SUBSYSTEM correct?
ClaudeCode/NOLF/harness/checkcontract.sh     # did the RUN drift?
```

⚠️ **Three different questions, and the middle one is not redundant.** The
contract catches drift but cannot say what *correct* is — it would hold just as
firmly around a string table that returned the wrong string every time, as long
as it did so consistently. `enginetests.sh` asserts correctness against known
values with a negative control for each; `checkcontract.sh` asserts sameness.

`checkcontract.sh` exits 0 when the run matches and 1 with a unified diff when
it does not. Run it after **every** change that touches the engine.

### Blessing a new reference

```bash
ClaudeCode/NOLF/harness/checkcontract.sh --bless
```

⚠️ **This is the dangerous command.** It makes whatever the code does right now
"correct" by definition. Do it only after reading the diff and deciding every
line was intended — the NOLF 2 contract's entire value came from people *not*
doing this reflexively. `--bless` prints the diff it is about to accept, so
there is no excuse for not reading it.

---

## What the record contains, and why each part is there

```
host   mode client                                 what was run
tree   rez nolfgoty.rez 387                        the archive stack, in add order
stub   ILTClient::SetRenderMode                    first call to an engine method
file   Attributes\Fx.txt 339310 nolfu003.rez       name, SIZE, and SOURCE ARCHIVE
miss   Attributes\Nothing.txt                      a file the game asked for and did not get
count  files-opened 11                             totals
```

Three deliberate choices:

* **`file` records size and source archive, not just the name.** The name alone
  would pass while the *wrong archive* served the file — and which copy wins is
  the entire purpose of the tree stack. `Fx.txt` from `nolf2.rez` and from
  `nolfu003.rez` differ by 5,003 bytes, and only this line tells them apart.
* **`stub` does not record the ordinal.** Recording `5 ILTCSBase::FormatString`
  would make one insertion renumber everything after it, so a single new call
  would show up as a hundred changed lines — an unreadable diff exactly when it
  matters most. Position in the file already carries the order.
* **`count` lines are not redundant.** The event lines say *what* happened once
  each; the counts say *how many times*. A retry loop that runs twice as often
  produces an identical event list and a different count.

### Determinism is the whole value

A record containing a timestamp, a pointer, a PID, an absolute path or a
hash-iteration order is not a contract — it is a random number generator that
fails every run and gets switched off within a week. Nothing written through
`LTRecord_Event` may contain any of those; see `engine/kernel/record/ltrecord.h`.

`selftest.sh` verifies this by running the host twice and requiring
byte-identical records.

---

## ⚠️ selftest.sh is the most important file here

The background doc §3.5 records three separate occasions on which the NOLF 2
port produced **confident wrong answers from bad instruments** — including an
envelope cross-correlation that scored a *negative control* highest.

A harness is not exempt from needing tests; it is the thing most in need of
them, because once it is wrong every answer it gives afterwards is wrong *and
confident*.

It earned this on its first execution: `framediff.py` used
`Image.getdata().index()`, which raises `AttributeError` because an
`ImagingCore` is sequence-like enough to look right and has no `.index()`. That
line only runs when something actually *differs* — so it would have been fine
through every passing run, and would have crashed on the first real regression.

### Both halves have negative controls, and that is not optional

* A differ that always returns 0 passes "identical images are identical".
  So a **one-pixel, one-unit** change must *fail* — a mean of 0.000163 that
  still trips the gate.
* An empty record diffed against an empty reference passes forever.
  So a **deliberately perturbed run** (one data directory instead of three)
  must *break* the contract. It differs by 33 lines.
* `half black / half white` must read **exactly 127.500** — a known analytic
  value, so the metric is checked against arithmetic and not just against
  itself.

---

## Scope, honestly stated

**What it covers today:** which engine methods the game reaches and in what
order; which files it opens, how big they are, and which archive served them;
whether the run completes.

**What it does not cover yet:** anything rendered (nothing renders), anything
timing-dependent (deliberately — times are not recorded), the server side (the
host is a client-only link, `HANDOVER.md` §4.8), and repeat-call counts per
method (only distinct first calls are recorded, plus grand totals).

⚠️ **The run has a side effect.** The game creates a `Save` directory in its
working directory (`GameClientShell.cpp:1015`). `runrecord.sh` therefore runs
the host in a throwaway directory — which also removes a determinism hazard,
since `DirExist("Save")` answers differently on the second run in one place.
See `HANDOVER.md` §4.9.

**The reference is tied to one data layout.** It names `GOTY-disc1/Data`,
`GOTY-disc2/Data` and `GOTY-disc2/Game`, and which archives are present in them.
Pointed anywhere else, the contract will correctly report a mismatch — that is
the negative control working, not a bug. Re-bless deliberately if the data moves.

## When frames arrive

Add `reference/frames/` alongside `reference/skeleton_client.record`, archive
the first known-good dumps there, and extend `checkcontract.sh` to call
`framediff.py` over them. The differ is already validated, so the only new
question that day will be about the renderer.
