# S-YXG100 Hybrid Research Wrapper

This source-only project tests a compatibility architecture for combining a
user-supplied 32-bit Yamaha S-YXG50 VST with separately recovered VL/PVL audio.
It does not contain or distribute Yamaha executables, tables, presets, or demo
files.

The current milestone is a transparent VST2 pass-through wrapper. Place a
lawfully retained portable S-YXG50 VST beside the wrapper under the filename
`syxg50-engine.dll`. The wrapper loads that engine and forwards its lifecycle,
MIDI, parameter, and audio calls unchanged. This provides a measurable baseline
before VL routing or mixing is added.

The baseline probe currently renders a real MIDI note through the wrapped
S-YXG50 engine with a finite nonzero output peak. `MidiRouter` separately tracks
all 16 MIDI channels and identifies VL/PVL bank MSBs 33, 81, and 97, including
the transition back to an ordinary XG bank. The router is tested but is not yet
connected to a production VL worker.

The intended next stage is a 64-bit out-of-process VL worker using fixed-size,
preallocated IPC buffers. This is required because the Yamaha child VST is
32-bit while the currently validated CPU-emulation runtime is 64-bit. No Python
process or dynamic allocation belongs on the VST audio callback.

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
