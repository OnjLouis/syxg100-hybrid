# S-YXG100 Hybrid Preservation Tester Notes

This is a validated preservation build for controlled testing. It is a working
32-bit VST2 instrument and has been exercised in real time in both VSTHost and
64-bit Foobar2000 through MIDI Player's bundled 32-bit VST bridge.

## Before testing

Read `README.html` for the project's background, runtime layout, and known gaps.
The private tester archive includes the three required Yamaha runtime files
recovered from an entitled installation. They must remain beside the VST:

```text
syxg50-engine.bin
Sxgpvknl.vxd
sxgsgknl.vxd
```

The archive groups the VST, both workers, and Yamaha runtime files in its
`VST` folder. Keep that folder intact. Add `syxg100-hybrid.dll` from there to
the host as **S-YXG100 Hybrid**. Its VST ID is `S1HY`, so it can coexist with
an original S-YXG50 entry.

## Current confidence

Host rates of 44.1 and 48 kHz are supported. The 44.1 kHz path retains its
accepted sample-identical output; complete VL and SG trace renders pass through
the new 48 kHz streaming adapter. Live Foobar2000 playback at 48 kHz confirms
that the former worker stall and crash are gone.
Ordinary XG, legacy monophonic VL, assignment-defined multi-channel PVL, SG,
native gain, reverb, and chorus have passed focused and real-time tests. Broader
compatibility is not complete. Fast VL pitch bends pass real-time comparison
after ordered RPN setup replay was corrected. Legacy VL songs without Yamaha
voice assignments retain their original channel and one-voice behaviour.
Assignment-defined PVL uses an eight-worker pool with exact note ownership and
oldest-note stealing. `Tableaux` and `VLJzAcid` passed genuine-VL listening
tests after that distinction was corrected. `UnderGrd` passed an
audible polyphony test and `PRckStar` passed comparison with its Windows 9x
reference render. The final `SG_yuki` regression passed in the live Foobar host
with no audible defect. Concurrent worker rendering also completed more than
one minute of the controller-heavy `StarStrp` without buffering. Processed
variation effects remain unverified because the retained controller-only probe
did not configure an XG variation algorithm and routing with SysEx.

`Oxygen` passed a custom-VL-voice test after first-worker setup was corrected
to preserve Yamaha model `0x57` uploads. Its VL-to-XG transition also exercises
retained note releases across bank changes. `Dogroova` subsequently passed a
live playback and transport re-entry test with no missing first note.
The reverse XG-to-VL transition is also covered: a held XG note now receives
its eventual note-off after entering VL. Sustain release and all-notes-off use
the same ownership-safe routing, and the correction passed a live QWS test.

## Useful reports

When reporting a problem, include the host and version, sample rate, MIDI file,
the approximate playback position, and whether the symptom affects ordinary
XG, VL/PVL, SG, or an effect. Listen for missing or stuck notes, incorrect
banks, timing changes, low level, missing tails, stale controller state after
GM1, GM2, GS, or XG reset messages, seek failures, and problems after switching
repeatedly between songs.

This private bundle includes Yamaha-derived runtime and demonstration files for
compatibility testing. Do not republish those files without the necessary
rights.
