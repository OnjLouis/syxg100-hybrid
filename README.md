# S-YXG100 Hybrid Research Wrapper

This source-only project combines a user-supplied 32-bit Yamaha S-YXG50 VST
with separately recovered VL/PVL synthesis. It does not contain or distribute
Yamaha executables, tables, presets, demo files, or firmware.

The wrapper keeps S-YXG50 as the proven AWM and effects engine. It routes MIDI
bank MSBs 33, 81, and 97 to a native PVL engine and leaves ordinary XG parts on
S-YXG50. Returning a channel from a VL bank to an ordinary bank clears the VL
part and forwards the transition to XG. GM and XG System On messages reset the
remembered routing state.

PVL runs as native 32-bit code in a small worker process. Process isolation is
required because the two legacy Yamaha engines otherwise overwrite shared
generated-callback state and crash when XG and VL notes are active together.
The worker uses inherited anonymous handles and shared memory; it does not use
network access, temporary files, Python, or CPU emulation. If the worker or VxD
is absent or fails, the wrapper remains a transparent XG-only pass-through.

The recovered renderer provides four signed 16-bit stereo planes. Controlled
MIDI-send tests identify them as dry, reverb return, chorus return, and
variation return. The wrapper sums all four into S-YXG50's floating-point
stereo output. MIDI events retain their VST block offsets, including the first
VL note, and render buffers and queues are fixed in size.

## Runtime Layout

Keep these files beside one another in a disposable test VST directory:

```text
syxg100-hybrid.dll       built by this project
syxg100-vl-worker.exe    built by this project
syxg50-engine.dll        user-supplied S-YXG50 VST
Sxgpvknl.vxd             user-supplied original PVL VxD
```

Neither user-supplied Yamaha file belongs in this repository or a distributed
source or binary package.

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

`NativeProbe` validates the in-process engine independently of S-YXG50:

```text
VlNativeProbe <Sxgpvknl.vxd> <events.pvte.txt>
```

`SgNativeProbe` is an experimental research harness for a user-supplied
original SG kernel. It validates multi-object LE loading, initialization,
bounded rendering, MIDI/SysEx transport, and clean shutdown. Native event
replay is stable, but audible SG synthesis is not yet implemented or claimed.

```text
SgNativeProbe <sxgsgknl.vxd> [events.sgte.txt] [output.wav]
```

The reference 2,048-frame render is deterministic and remains comfortably
faster than real time. The older 64-bit Unicorn worker remains only as a
research oracle; it is not part of the wrapper's runtime path.
