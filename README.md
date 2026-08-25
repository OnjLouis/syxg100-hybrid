# S-YXG100 Hybrid Research Wrapper

This source-only project tests a compatibility architecture for combining a
user-supplied 32-bit Yamaha S-YXG50 VST with separately recovered VL/PVL audio.
It does not contain or distribute Yamaha executables, tables, presets, or demo
files.

The first milestone is a transparent VST2 pass-through wrapper. Place a
lawfully retained portable S-YXG50 VST beside the wrapper under the filename
`syxg50-engine.dll`. The wrapper loads that engine and forwards its lifecycle,
MIDI, parameter, and audio calls unchanged. This provides a measurable baseline
before VL routing or mixing is added.

The baseline probe currently renders a real MIDI note through the wrapped
S-YXG50 engine with a finite nonzero output peak. `MidiRouter` separately tracks
all 16 MIDI channels and identifies VL/PVL bank MSBs 33, 81, and 97, including
the transition back to an ordinary XG bank. The router is tested but is not yet
connected to a production VL worker.

The second milestone proves that the recovered PVL engine can initialize and
render directly as native 32-bit code. `NativeProbe` maps a separately prepared,
user-supplied PVL image, applies the compatibility layer required by its Windows
9x VxD assumptions, replays a captured MIDI event trace, and renders through a
preallocated low-address workspace. Ten clean-process runs of the reference
trace produced identical plane statistics. A 2,048-frame block took about 5.9
to 6.5 ms on the development system, comfortably below the 46.4 ms represented
by that block at 44.1 kHz.

`Worker` retains the earlier 64-bit Unicorn implementation as a research oracle.
It established correct output before native execution was understood, but its
roughly 18-times-slower-than-real-time performance rules it out as the shipping
renderer. The production direction is now an in-process 32-bit native VL engine
with fixed-size buffers. The source-only `LeImageLoader` now reconstructs and
relocates the original user-supplied `Sxgpvknl.vxd` in memory; this repository
contains neither a relocated image nor any Yamaha binary data.

## Build

Build output must remain outside this source directory. The project targets
32-bit Windows because the original Yamaha VST is 32-bit.

```text
cmake -S . -B <build-directory> -G Ninja \
  -DCMAKE_C_COMPILER=<i686-clang> \
  -DCMAKE_CXX_COMPILER=<i686-clang++>
cmake --build <build-directory>
```

`HybridHostProbe.exe <wrapper.dll>` opens the wrapper, checks the VST identity,
sets 44.1 kHz and a 512-frame block, enables processing, renders one silent
block, and closes it. A successful result confirms the wrapper and child engine
completed their normal VST lifecycle.

`NativeProbe` is a research executable rather than an end-user conversion tool.
It accepts the user's original PVL VxD and a captured `.pvte.txt` event trace:

```text
VlNativeProbe <Sxgpvknl.vxd> <events.pvte.txt>
```

Successful reference output has nonzero audio on planes 1, 2, and 4, a silent
plane 3, and a render time well below real time. Exact peaks can vary when the
input trace changes.
