# S-YXG100 Hybrid

## Version 0.1.4

Corrects an SG startup memory error that could crash the singing worker under
Wine. The legacy one-slot setup now supplies the one-based slot number expected
by Yamaha's native helpers. The loaded image is checked before this correction
is applied, and the Yamaha file on disk remains unchanged.

## Version 0.1.3

Prepares the eight VL helpers and the SG helper in a serialized, below-normal-
priority background phase when the host activates the plug-in. This removes
process creation and Yamaha-engine initialization from first-note MIDI
processing. A single active plug-in instance reserves approximately 60-70 MB
for its ready helpers; hosts that prebuffer more than one instance multiply
that figure.

## Version 0.1.2

Adds an accessible native editor and handles complete Yamaha model `0x64`
Plug-in Voice bulk transactions. Songs can now load and change their embedded
VL voices during playback, including the associated bank, program, volume,
mono/poly, pitch-bend range, portamento, reverb, and chorus settings.

For the user-facing overview, runtime layout, current compatibility notes, and
real-time host instructions, see [`README.html`](README.html).

This source-only project combines a user-supplied 32-bit Yamaha S-YXG50 VST
with separately recovered VL/PVL synthesis. It does not contain or distribute
Yamaha executables, tables, presets, demo files, or firmware.

S-YXG100 Hybrid brings the discontinued Windows 9x VL/PVL and SG engines into
a modern Windows VST2 host while retaining S-YXG50 for ordinary XG synthesis
and Yamaha effects. The recovered engines run as isolated worker processes so
their legacy generated-code state cannot corrupt the host or one another.

## Accessible editor

The plug-in editor opens a native Windows dashboard with three pages: live
status, under-the-hood routing, and the original Yamaha editor. Status uses
standard named controls and a 16-channel report list that exposes engine,
bank, program, note activity, controllers, pitch bend, and effect sends to
screen readers. Its small engine activity graphic is supplemental; the same values
are always present in the selected-channel text.

Use `Alt+S`, `Alt+U`, or `Alt+Y` for the three pages, `F5` or `Alt+R` to
refresh, and `Alt+C` to copy a complete text report. Tab and Shift+Tab move
through the controls, and arrow keys switch among the page buttons. Live
updates change rows in place without rebuilding the list or moving focus.

The status page describes the live plug-in instance owned by its host. VSTHost
therefore exposes current channel activity. Foobar2000 MIDI Player may render a
MIDI file ahead of playback and later open the editor on a new, idle instance;
in that host the status page can consequently show defaults while Foobar2000 is
playing its already-rendered audio.

The wrapper keeps S-YXG50 as the proven AWM and effects engine. It routes MIDI
bank MSBs 33, 81, and 97 to a native PVL engine and leaves ordinary XG parts on
S-YXG50. Returning a channel from a VL bank to an ordinary bank clears the VL
part and forwards the transition to XG. GM1, GM2, GS, and XG system reset
messages reset the remembered routing state.

Channel 10 retains the normal rhythm default after a reset. In XG mode, a later
bank MSB changes that implicit mode: melodic banks release channel 10 from
rhythm operation, while bank 127 selects drums. GM1 and GS bank selections do
not override their channel-10 drum default; their original bank, program, and
drum-note layout pass through to S-YXG50 unchanged. GM2 follows its standard
bank 120 rhythm and bank 121 melodic selections. An explicit Yamaha part-mode SysEx
remains authoritative and is not overridden by subsequent bank changes.

Note ownership is retained across bank changes in both directions. If a note,
sustain pedal, or all-notes-off transaction begins under one engine and its
channel changes between XG and VL before release, the required release reaches
both engines. Ordinary events remain routed only to their current owner.

VL/PVL runs as native 32-bit code in a pool of eight one-voice worker
processes prepared in the background when the host activates the plug-in.
Legacy VL files without Yamaha voice-assignment SysEx retain
their original source channel and the monophonic behaviour of S-YXG100LE.
Japanese PVL files explicitly assign native voice slots; only that mode maps
workers onto Yamaha's canonical first VL part and enables up to eight-note
polyphony. Notes retain exact voice ownership across overlapping and repeated
pitches; a ninth simultaneous note steals the oldest active voice. A compact
per-channel snapshot
restores bank, program, controllers, RPN/NRPN state, pressure, and pitch bend
when a worker changes channels. XG/PVL part SysEx is filtered and remapped in
the same way while global SysEx remains unchanged. Process isolation prevents
the legacy Yamaha engines from overwriting shared generated-callback state.

Yamaha model `0x5D` SysEx routes singing data to a separately prepared native
SG worker. The wrapper
replays bounded pre-activation setup with its original timing, preserves MIDI
and SysEx order and block offsets, queries the SG route mask, and suppresses
only note-on/off events that SG accepts. SG never shares an address space with
XG50 or PVL.

Workers use inherited anonymous handles and shared memory; they do not use
network access, temporary files, Python, or CPU emulation. Timed MIDI and audio
are batched once per render block: all active VL workers run concurrently, then
the wrapper waits and mixes their completed buses. The callback-stack keeper is
suspended while idle, avoiding one busy-spinning thread per worker. If a worker
or VxD is absent or fails, native routing falls back without taking down XG.

Both recovered renderers provide four signed 16-bit stereo planes. Controlled
CC91, CC93, and CC94 impulse tests identify them as dry plus unprocessed reverb,
chorus, and variation send buses. A signature-checked bridge inserts all four
native planes between XG50 voice synthesis and its original effects stage,
converting native normalized samples to XG50's internal sample-unit scale.
XG50 bus probes confirm dry at buses 0/1, reverb at 2/3, chorus at 4/5, and
variation at 6/7. Reverb and chorus then use the same Yamaha processing as XG
parts; variation still depends on XG50's effect configuration and connection
mode. If that exact XG50 build is unavailable or the bridge signature does not
match, the wrapper mixes the native dry plane directly instead. A calibrated
native gain of 3.5 is applied in either path. MIDI events retain their VST block
offsets, including the first VL note. PVL exposes its generated renderer only
after an initial native trigger. The wrapper retains a bounded setup history,
uses the first positive VL note to warm the native path, replays setup because
warm-up consumes that state, and retriggers the note at its original block
offset. Audio rendering remains dormant until then.

The pre-activation setup history preserves every short MIDI message in exact
arrival order. This is required for stateful RPN and NRPN transactions: treating
CC101, CC100, and Data Entry as independent replaceable controller values can
turn a valid pitch-bend-range sequence into an ineffective RPN-null sequence.
The first worker consumes that ordered history directly without appending a
redundant channel snapshot. This also preserves custom model `0x57` VL voice
uploads, which can be replaced if bank and program selection are sent again
after the upload. Workers created later still receive the current snapshot.
The retained history remains fixed at 1,024 events and never allocates in the
audio callback.

PVL uses Yamaha's genuine gate-zero software renderer through dispatcher service
7. Every native render uses a fixed 256-frame cadence, independent of the host
block size. The older generated hardware-transport path remains available only
for diagnostic comparison with `SYXG100_VL_RENDER_PATH=transport`; it is not the
default. SG uses its recovered 256-frame native host bridge. Render buffers,
event queues, and IPC storage are fixed in size.

VL and SG always run at their proven native rate of 44,100 Hz. At a different
host rate, a preallocated streaming adapter converts all eight native dry and
effect-send buses while retaining fractional position and unused source samples
across callbacks. Absolute MIDI positions are converted onto the same native
timeline, avoiding per-block rounding drift. The 44.1 kHz path bypasses the
adapter and remains sample-identical to the accepted build. Complete VL and SG
trace renders have also passed at 48 kHz. Real-time playback at 48 kHz has been
confirmed in Foobar2000 without the former worker stall or crash.

## Runtime Layout

The packaged tester archive groups these files in its `VST` folder. Keep them
beside one another in that one runtime directory:

```text
syxg100-hybrid.dll       built by this project
syxg100-vl-worker.exe    built by this project
syxg100-sg-worker.exe    built by this project
syxg50-engine.bin        user-supplied S-YXG50 VST binary
Sxgpvknl.vxd             user-supplied original PVL VxD
sxgsgknl.vxd             user-supplied original SG VxD
syxg100-hybrid.version.json  updater product/version marker
```

The user-supplied Yamaha files do not belong in this repository or a
distributed source or binary package.

## Wine compatibility

The 32-bit plug-in and its separate native VL/SG workers can run under Wine,
but the workers require Wine's newer WoW64 process arrangement. A user reported
that an incompatible prefix let ordinary XG play while the VL worker faulted
as soon as a VL note sounded, causing the part to fall back to piano. For Wine
11, use a 64-bit prefix and force the newer WoW64 mode when launching the
32-bit VST host, for example:

```sh
WINEPREFIX="$HOME/.wine-syxg" WINEARCH=win64 wineboot
WINEPREFIX="$HOME/.wine-syxg" WINEARCH=wow64 wine /path/to/vst-host.exe
```

`WINEARCH=wow64` requires a prefix that was created as 64-bit, which is Wine's
default. It cannot convert a pure `WINEARCH=win32` prefix; create a separate
64-bit prefix instead. This workaround was reported in
[issue 4](https://github.com/OnjLouis/syxg100-hybrid/issues/4) and agrees with
[Wine 11's documented new-WoW64 behaviour](https://list.winehq.org/hyperkitty/list/wine-releases%40list.winehq.org/thread/UL6L2GJ55VYUJ5KUMBZ3TZSXRFJ52QG6/).
Wine remains community-tested rather than a primary supported platform.

## Updater

`Update Yamaha Hybrids.cmd` launches the shared PowerShell updater. The pair
may be placed in this product folder or in a common parent containing both
hybrids. It searches at most two folder levels, detects existing installations
by their unique wrapper DLLs, and never installs a missing synth.

Stable GitHub release metadata is signed with a product-specific RSA key. The
manifest restricts replacement to an allowlist, identifies the exact product,
and supplies SHA-256 hashes for the externally hosted runtime ZIP and every
managed file. The updater stages and verifies the complete package before
changing the live folder. It displays release notes and requires confirmation
unless explicitly invoked for unattended installation.

Before replacement, managed files are stored in one rollback ZIP beneath
`%LOCALAPPDATA%\Onj Research\Yamaha Hybrid Updater\syxg100-hybrid\backups`.
Only the newest rollback and two bounded logs are retained. Rollback DLLs are
never left loose in a VST search path, unrelated files are preserved, and a
partial replacement is restored automatically. Audio hosts must be closed so
they do not lock the runtime files.

## Portability and macOS

The repository is intentionally public so the preservation work can be studied,
verified, and extended. The MIDI routing, ordered setup history, voice allocator,
effects-bus mapping, LE image loader, native-engine interfaces, and behavioural
tests provide a concrete starting point for another platform.

The current runtime is not directly portable: it loads original 32-bit Windows
PE/LE code and depends on Win32 process, shared-memory, event, and VST2 APIs.
A native macOS port would need replacements for that hosting layer and either a
compatible execution environment for the entitled legacy runtime or a clean
reimplementation of the recovered synthesis interfaces. Yamaha binaries and
demonstration files are not licensed by this project and must not be committed
to a fork or redistributed with a build.

## Build

Build output must remain outside this source directory. The wrapper and worker
target 32-bit Windows because the original engines are 32-bit.

```text
cmake -S . -B <build-directory> -G Ninja \
  -DCMAKE_C_COMPILER=<i686-clang> \
  -DCMAKE_CXX_COMPILER=<i686-clang++>
cmake --build <build-directory>
ctest --test-dir <build-directory> --output-on-failure
```

## Verification Tools

`HybridHostProbe.exe <wrapper.dll>` verifies the normal XG path. Supplying a
captured event trace also exercises VL routing and mixing:

```text
HybridHostProbe <wrapper.dll> [events.pvte.txt]
```

`HYBRID_PROBE_BLOCKS` controls the number of post-event render blocks.
`HYBRID_PROBE_NOTE_DELTA` assigns a block offset to trace note events for timing
checks. Event traces and Yamaha-derived test data are private research inputs
and are not included here.

The wrapper has also been exercised in 32-bit VSTHost with its real-time audio
engine. Load the wrapper from an isolated runtime directory, then use VSTHost's
MIDI player or a virtual MIDI input to send complete songs. Confirm that no more
than eight VL voice workers exist, SG owns one separate worker, playback time
advances normally, and all workers exit when the host closes. A virtual MIDI
loop is required when testing from an external sequencer such as QWS.

For native-render diagnostics, `SYXG100_VL_GENERATED_HEAP=restore` retains the
current deterministic heap-cursor behaviour. Setting it to `advance` allows the
generated data heap to advance naturally with the generated code ring. This is
an experimental comparison switch, not a release setting.
`SYXG100_VL_GENERATED_JITTER=zero` retains the deterministic timestamp patch;
`native` preserves Yamaha's original timestamp-derived allocator advance. The
latter can vary between worker launches and is also diagnostic-only.

`NativeProbe` validates the in-process engine independently of S-YXG50:

```text
VlNativeProbe <Sxgpvknl.vxd> <events.pvte.txt>
```

`SgNativeProbe` independently validates a user-supplied original SG kernel,
including multi-object LE loading, initialization, bounded rendering,
MIDI/SysEx transport, native host mixing, and clean shutdown.

```text
SgNativeProbe <sxgsgknl.vxd> [events.sgte.txt] [output.wav]
```

The reference 2,048-frame render is deterministic and remains comfortably
faster than real time. The older 64-bit Unicorn worker remains only as a
research oracle; it is not part of the wrapper's runtime path.

Set `SYXG100_DISABLE_XG_EFFECTS=1` only to compare the direct dry fallback.
`SYXG100_NATIVE_GAIN` overrides the calibrated default native gain of `3.5` for
diagnostic comparisons; accepted values are `0.1` through `8.0`. XG50 can emit
floating-point samples above unity even with all native channels muted, so a
host or lossless test renderer must preserve headroom before final conversion.
`SYXG100_HYBRID_LOG` enables bounded wrapper diagnostics, and
`SYXG100_SG_WORKER_LOG` writes one end-of-run SG worker summary. None of these
diagnostic settings is required for normal use.
