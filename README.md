# S-YXG100 Hybrid

For the user-facing overview, runtime layout, current compatibility notes, and
real-time host instructions, see [`README.html`](README.html).

This source-only project combines a user-supplied 32-bit Yamaha S-YXG50 VST
with separately recovered VL/PVL synthesis. It does not contain or distribute
Yamaha executables, tables, presets, demo files, or firmware.

S-YXG100 Hybrid brings the discontinued Windows 9x VL/PVL and SG engines into
a modern Windows VST2 host while retaining S-YXG50 for ordinary XG synthesis
and Yamaha effects. The recovered engines run as isolated worker processes so
their legacy generated-code state cannot corrupt the host or one another.

The wrapper keeps S-YXG50 as the proven AWM and effects engine. It routes MIDI
bank MSBs 33, 81, and 97 to a native PVL engine and leaves ordinary XG parts on
S-YXG50. Returning a channel from a VL bank to an ordinary bank clears the VL
part and forwards the transition to XG. GM and XG System On messages reset the
remembered routing state.

VL/PVL runs as native 32-bit code in a pool of eight lazily created one-voice
worker processes. Legacy VL files without Yamaha voice-assignment SysEx retain
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

Yamaha model `0x5D` SysEx lazily starts a separate native SG worker. The wrapper
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

## Runtime Layout

Keep these files beside one another in one VST runtime directory:

```text
syxg100-hybrid.dll       built by this project
syxg100-vl-worker.exe    built by this project
syxg100-sg-worker.exe    built by this project
syxg50-engine.bin        user-supplied S-YXG50 VST binary
Sxgpvknl.vxd             user-supplied original PVL VxD
sxgsgknl.vxd             user-supplied original SG VxD
```

Neither user-supplied Yamaha file belongs in this repository or a distributed
source or binary package.

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
