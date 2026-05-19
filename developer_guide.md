# TI FSI Analyzer — Developer Guide

**Author:** Nima Eskandari

## Overview

This is a Saleae Logic 2 Low-Level Analyzer (LLA) plugin that decodes Texas
Instruments Fast Serial Interface (FSI) frames. It is written in C++14 and
built as a shared library loaded by Logic 2 at runtime.

---

## Repository layout

```
fsi_analyzer/
├── CMakeLists.txt                  — top-level build file
├── CLAUDE.md                       — project overview and orientation
├── FSIFrame.pdf                    — TI FSI TRM extract (protocol reference)
├── AnalyzerSDK/                    — bundled Saleae Analyzer SDK (pre-built, do not modify)
│   ├── include/                    — SDK headers
│   │   ├── Analyzer.h              — Analyzer2 base class, UseFrameV2()
│   │   ├── AnalyzerChannelData.h   — AnalyzerChannelData (channel reads)
│   │   ├── AnalyzerResults.h       — Frame, FrameV2, AnalyzerResults base
│   │   ├── AnalyzerSettings.h      — AnalyzerSettings base, SimpleArchive
│   │   ├── AnalyzerSettingInterface.h — UI widget interfaces
│   │   ├── AnalyzerHelpers.h       — GetNumberString(), etc.
│   │   └── ...
│   ├── lib_x86_64/                 — pre-built SDK library (Linux x86-64 / Windows x64)
│   ├── lib_arm64/                  — pre-built SDK library (macOS arm64)
│   └── testlib/                    — SDK unit-test harness (MockChannelData, etc.)
├── SampleAnalyzer/                 — upstream Saleae example project (reference only)
├── src/
│   ├── FSIAnalyzer.h / .cpp        — main analyzer: WorkerThread, SyncPreamble,
│   │                                 CollectBits, CRC, frame emission
│   ├── FSIAnalyzerSettings.h / .cpp — Logic 2 settings UI and serialization
│   └── FSIAnalyzerResults.h / .cpp  — bubble text, tabular text, CSV export
├── final_release/                  — latest built plugin binaries
│   ├── FSIAnalyzer.so              — Linux x86-64
│   └── FSIAnalyzer.dll             — Windows x64
└── build/                          — CMake build output (not tracked)
    └── Analyzers/
        └── FSIAnalyzer.so
```

---

## Architecture

The plugin follows the Saleae LLA pattern: three classes derived from SDK
base classes, plus three C-linkage entry points.

```
Analyzer2        ←  FSIAnalyzer          (WorkerThread: parse bits → emit frames)
AnalyzerSettings ←  FSIAnalyzerSettings  (UI: channel pickers, dropdowns)
AnalyzerResults  ←  FSIAnalyzerResults   (render frames as text / CSV)
```

### Key call flow

```
Logic 2 loads .so → CreateAnalyzer() → FSIAnalyzer()
                                         SetAnalyzerSettings()
                                         UseFrameV2()
                  → SetupResults()
                  → WorkerThread() [runs on background thread]
                       loop:
                         SyncPreamble()              — scan for ≥5 HIGH bits + SOF (1001)
                                                       emits FSI_RESULT_PREAMBLE (4 preamble bits)
                                                       emits FSI_RESULT_SOF      (SOF[0]..SOF[3])
                         CommitPacketAndStartNewPacket()
                         CollectBits(4, …, false)    — Frame Type (control field)
                                                       emits FSI_RESULT_FRAME_TYPE
                         [data frames only:]
                           CollectBits(8, …, true)   — User Data (interleaved)
                                                       emits FSI_RESULT_USERDATA
                           CollectBits(16, …, true) × N — Data Words (interleaved)
                                                       emits FSI_RESULT_DATA_WORD × N
                           CollectBits(8, …, true)   — CRC (interleaved)
                                                       emits FSI_RESULT_CRC
                         CollectBits(4, …, false)    — Frame Tag (control field)
                                                       emits FSI_RESULT_TAG
                         CollectBits(4, …, false)    — EOF pattern (control field)
                                                       emits FSI_RESULT_EOF or FSI_RESULT_ERROR
                         CollectBits(4, …, false)    — Postamble
                                                       emits FSI_RESULT_POSTAMBLE
                         CommitResults()
```

Each field calls `mResults->AddFrame()` and `mResults->AddFrameV2()` immediately
after collection, not at the end of the frame.

### FSI frame structure (TRM Table 31-4 / §31.3.4.1)

```
Idle        : CLK=HIGH, D0=HIGH, D1=HIGH — no clock edges
Preamble    : 4 clock edges, data HIGH  (= 1111)
SOF         : 4 bits = 1001             — detected by SyncPreamble, emits FSI_RESULT_SOF (SOF[0..3])
Frame Type  : 4 bits  [control field]
[data frames only]
  User Data : 8 bits  [interleaved in 2-lane]
  Data Words: N×16b   [interleaved in 2-lane]
  CRC       : 8 bits  [interleaved in 2-lane]
Frame Tag   : 4 bits  [control field]
EOF         : 4 bits = 0110  [control field]
Postamble   : 4 clock edges, data HIGH  (= 1111)  — emits FSI_RESULT_POSTAMBLE
Idle        : CLK=HIGH, no edges
```

### Frame type codes (TRM Table 31-5)

| Constant | Hex | Value (binary) | Frame type |
|---|---|---|---|
|`FSI_FRAME_TYPE_PING` |0x0|0000|PING — heartbeat, no data|
|`FSI_FRAME_TYPE_NWORD`|0x3|0011|DATA(Nw) — N data words (settings-configured)|
|`FSI_FRAME_TYPE_DATA1`|0x4|0100|DATA(1w) — 1 data word|
|`FSI_FRAME_TYPE_DATA2`|0x5|0101|DATA(2w) — 2 data words|
|`FSI_FRAME_TYPE_DATA4`|0x6|0110|DATA(4w) — 4 data words|
|`FSI_FRAME_TYPE_DATA6`|0x7|0111|DATA(6w) — 6 data words|
|`FSI_FRAME_TYPE_ERROR`|0xF|1111|ERROR — error signalling, no data|

All other 4-bit codes are reserved. `IsDataFrame()` returns true for NWORD,
DATA1, DATA2, DATA4, DATA6. `DataWordCount()` returns the word count for
each type, or 0 for PING, ERROR, and unknown codes.

### Frame result types (defined in `FSIAnalyzer.h`)

Ordered as they appear in a decoded frame:

| Constant | Value | Emitted for |
|---|---|---|
|`FSI_RESULT_PREAMBLE`   |0x00|Preamble (4 HIGH bits) — bubble spans all 4 preamble bits|
|`FSI_RESULT_FRAME_TYPE` |0x01|4-bit frame type field|
|`FSI_RESULT_TAG`        |0x02|4-bit frame tag field|
|`FSI_RESULT_USERDATA`   |0x03|8-bit user data field (data frames only)|
|`FSI_RESULT_DATA_WORD`  |0x04|Each 16-bit data word (`mData2` = zero-based word index)|
|`FSI_RESULT_CRC`        |0x05|8-bit CRC (`mFlags` bit 0: 1=OK, 0=FAIL)|
|`FSI_RESULT_EOF`        |0x06|EOF pattern (0110) validated|
|`FSI_RESULT_SOF`        |0x07|SOF[0..3] — bubble spans all 4 SOF bits (1001)|
|`FSI_RESULT_POSTAMBLE`  |0x08|Postamble (4 HIGH bits)|
|`FSI_RESULT_ERROR`      |0xFF|Bad EOF pattern — `mData1` holds the received value|

### `mData1`, `mData2`, and `mFlags` usage

| Result type | `mData1` | `mData2` | `mFlags` |
|---|---|---|---|
|`FSI_RESULT_PREAMBLE`   | 0 | 0 | 0 |
|`FSI_RESULT_FRAME_TYPE` | frame type code (0x0–0xF) | 0 | 0 |
|`FSI_RESULT_TAG`        | tag value (0x0–0xF) | 0 | 0 |
|`FSI_RESULT_USERDATA`   | user data byte | 0 | 0 |
|`FSI_RESULT_DATA_WORD`  | 16-bit word value | zero-based word index | 0 |
|`FSI_RESULT_CRC`        | received CRC byte | computed (expected) CRC byte | bit 0: 1=match |
|`FSI_RESULT_EOF`        | 0x6 | 0 | 0 |
|`FSI_RESULT_SOF`        | 0 | 0 | 0 |
|`FSI_RESULT_POSTAMBLE`  | 0 | 0 | 0 |
|`FSI_RESULT_ERROR`      | received EOF value | 0 | 0 |

### Preamble detection and bubble boundaries

`SyncPreamble` scans for ≥ 5 consecutive HIGH bits on D0 (4 preamble bits +
SOF[0]), then reads ahead for SOF[1,2,3] = 0,0,1. A 5-slot ring buffer of the
most-recent HIGH sample positions allows the preamble bubble to be placed
precisely:

- **Bubble start** (`pre_start`): `s_ring[r % 5]` — the oldest slot, always
  the sample of preamble bit 1.
- **Bubble end** (`pre_end`): `s_ring[(r-1+5) % 5]` — the most-recent slot,
  always the sample of SOF[0].
- **SOF[1,2,3]** are consumed during the look-ahead check. A `FSI_RESULT_SOF`
  (0x07) bubble is emitted immediately after the preamble bubble, spanning
  SOF[1] to SOF[3].

The ring buffer overwrites the oldest entry on every new HIGH, so any number
of preceding idle or flush HIGHs are absorbed without special-case code.

**Important:** `WorkerThread` does not pre-advance the clock cursor at startup.
FSI idle is CLK=HIGH with no clock edges, so the cursor sits idle until the
first falling edge of the preamble. Pre-advancing would consume preamble bit 1
and cause the first frame to be missed.

### 2-lane interleaving (`CollectBits`)

```cpp
bool CollectBits( U32 count, U64& value,
                  U64& start_sample, U64& end_sample,
                  bool interleaved = true );
```

**`interleaved = false`** — control fields (Frame Type, Frame Tag, EOF, Postamble):
- Always reads `count` edges from TXD0 only.
- In 2-lane mode these fields are transmitted identically on both lanes; D0 suffices.

**`interleaved = true`** — data fields (User Data, Data Words, CRC):
- 1-lane: reads `count` edges from TXD0, MSB-first.
- 2-lane: reads `ceil(count/2)` edges. Each edge delivers two bits simultaneously:
  TXD0 → even-position bit (index 0, 2, 4 …), TXD1 → odd-position bit (index 1, 3, 5 …).
  Both are shifted into `value` MSB-first within the same edge.

### CRC

FSI uses CRC-8, polynomial `x^8 + x^2 + x + 1` (0x07), seed 0x00, no final
XOR. The lookup table `kFsiCrcTable[256]` in `FSIAnalyzer.cpp` implements this.

CRC input byte order:
1. User Data byte (1 byte)
2. Each data word: LSB byte first, then MSB byte (2 bytes per word)

Frame Type and Frame Tag are **not** included in the CRC.

---

## Build prerequisites

| Tool | Minimum version |
|---|---|
|CMake|3.13|
|GCC or Clang (Linux)|any C++14-capable|
|MSVC (Windows)|2019+ x64|
|Xcode / clang (macOS)|any C++14-capable|

The `AnalyzerSDK/` directory is bundled in the repository — no separate
download is required.

---

## Building

### Linux

```bash
git clone https://github.com/nimeskan/fsi_analyzer
cd fsi_analyzer
mkdir build && cd build
cmake ..
cmake --build .
```

Output: `build/Analyzers/FSIAnalyzer.so`

CMake auto-detects the SDK library: it prefers `AnalyzerSDK/lib_x86_64/libAnalyzer.so`
and falls back to `AnalyzerSDK/lib/libAnalyzer.so` if that path is absent.

### macOS

```bash
mkdir build && cd build
cmake ..
cmake --build .
xattr -d com.apple.quarantine build/Analyzers/FSIAnalyzer.dylib
```

Output: `build/Analyzers/FSIAnalyzer.dylib`

> **Note:** The bundled macOS library is in `AnalyzerSDK/lib_arm64/`. If you
> are on Intel macOS, update `SALEAE_LIB` in `CMakeLists.txt` to point at an
> x86-64 macOS build of `libAnalyzer.dylib` from the upstream Saleae SDK release.

### Windows (x64 Developer Command Prompt)

```bat
mkdir build && cd build
cmake .. -A x64
cmake --build . --config Release
```

Output: `build\Analyzers\Release\FSIAnalyzer.dll`

> **Note:** The bundled `AnalyzerSDK/lib_x86_64/Analyzer.lib` and `Analyzer.dll`
> are used. Update `SALEAE_LIB` / `SALEAE_DLL` in `CMakeLists.txt` if targeting
> ARM64 Windows.

### Important CMake flag

`add_definitions(-DLOGIC2)` is set unconditionally in `CMakeLists.txt`.
This preprocessor define unlocks `FrameV2`, `AddFrameV2`, and `UseFrameV2()`
in `AnalyzerResults.h`. Removing it will cause a build failure.

---

## Deploying to Logic 2

1. Build the plugin (see above) or use a pre-built binary from `final_release/`.
2. Open **Logic 2**.
3. Go to **Edit → Settings** and scroll to **Custom Low Level Analyzers**.
4. Click **+** and add the path to the folder containing the plugin file.
5. **Restart Logic 2** — the directory is only scanned at startup.
6. In a capture session, click **Analyzers → +** and search for **TI FSI**.

To update after a recompile, Logic 2 must be restarted. There is no hot-reload
mechanism for LLA plugins.

---

## Testing

The Saleae SDK ships a unit-test harness in `AnalyzerSDK/testlib/`. Key types:

| Class | Purpose |
|---|---|
|`AnalyzerTest::Instance`|Loads the analyzer and drives `WorkerThread`|
|`AnalyzerTest::MockChannelData`|Feeds synthetic edge data to channel inputs|
|`AnalyzerTest::MockResults`|Captures emitted `Frame` and `FrameV2` objects|

No automated tests are currently wired up in this project. To add them:

1. Create a `tests/` directory alongside `src/`.
2. Add a new `CMakeLists.txt` target that links against `AnalyzerSDK/testlib/`
   sources and the analyzer objects.
3. In test code, construct an `AnalyzerTest::Instance`, push synthetic clock
   and data edges via `MockChannelData`, call `RunAnalyzerWorker()`, then
   assert on `GetResults()`.

The highest-value test targets are `SyncPreamble` (preamble detection across
both capture scenarios and flush sequences), `CollectBits` (1-lane and 2-lane
interleaving), and `ComputeCRC` (known byte vectors with expected outputs).

### Manual verification workflow

1. Connect TXCLK and TXD0 (and TXD1 if 2-lane) to a Logic device running
   at ≥ 4× your FSI clock frequency.
2. Load the plugin and add the analyzer to the capture.
3. Inspect bubble labels: each FSI field should appear in order:
   `PRE → SOF → FT → [UD → Data words → CRC] → TAG → EOF → POST`.
4. Confirm the `SOF` bubble appears between `PRE` and `FT`, and the `POST`
   bubble appears at the end of each frame after `EOF`.
5. Check that CRC bubbles show **OK** on valid data frames.
6. Use **Analyzers → Export** to produce a CSV and compare field values
   against the firmware's transmitted data.

---

## Debugging

### File logging

Logic 2 does not expose a console for plugin output. The standard approach is
to write `fprintf` print statements to a log file and trace the program flow
by reading the output after a capture.

Add the include and a helper macro at the top of whichever source file you are
debugging:

```cpp
#include <cstdio>

#define FSI_LOG( ... ) do { \
    FILE* _f = fopen( "/tmp/fsi_debug.log", "a" ); \
    fprintf( _f, __VA_ARGS__ ); \
    fclose( _f ); \
} while(0)
```

Then insert `FSI_LOG` calls at the points you want to trace. For example, to
watch `SyncPreamble` process every clock edge:

```cpp
// Inside the SyncPreamble while loop, after reading b0:
FSI_LOG( "edge  sample=%-10llu  b0=%d  high_count=%u  r=%u\n",
         (unsigned long long)s, (int)b0, high_count, r );
```

To log the preamble commit point:

```cpp
FSI_LOG( "PRE COMMIT  pre_start=%llu  pre_end=%llu\n",
         (unsigned long long)pre_start, (unsigned long long)pre_end );
```

To trace each `CollectBits` result in `WorkerThread`:

```cpp
FSI_LOG( "CollectBits  count=%u  value=0x%llX  start=%llu  end=%llu\n",
         count, (unsigned long long)value,
         (unsigned long long)start_sample, (unsigned long long)end_sample );
```

**Workflow:**

1. Add `FSI_LOG` calls around the suspect code path.
2. Delete the old log: `rm /tmp/fsi_debug.log`
3. Rebuild: `cmake --build build/`
4. Restart Logic 2 and run a short capture.
5. Read the log: `cat /tmp/fsi_debug.log`

On Windows use `C:\Temp\fsi_debug.log` in place of `/tmp/fsi_debug.log`.
Remove all `FSI_LOG` calls before committing — they reopen the file on every
edge and will slow the analyzer significantly on long captures.

---

### Attaching GDB (Linux)

Because the plugin is a shared library loaded into Logic 2's process, GDB can
attach to Logic 2 and set breakpoints directly inside the analyzer code.

```bash
# 1. Build with debug symbols
cmake -DCMAKE_BUILD_TYPE=Debug build/
cmake --build build/

# 2. Start Logic 2, load a capture, then find its PID
pgrep -a Logic2

# 3. Attach GDB
gdb -p <pid>

# 4. Inside GDB — load the analyzer's symbols
(gdb) sharedlibrary FSIAnalyzer

# 5. Set breakpoints
(gdb) break FSIAnalyzer::SyncPreamble
(gdb) break FSIAnalyzer::CollectBits
(gdb) break FSIAnalyzer::WorkerThread

# 6. Resume and step through the decode
(gdb) continue
```

Once stopped inside `SyncPreamble` you can inspect the ring buffer directly:

```
(gdb) print high_count
(gdb) print r
(gdb) print s_ring
(gdb) print pre_start
(gdb) print pre_end
```

---

### Regression testing with testlib

Once you identify a failing input, encode it as a synthetic test using
`AnalyzerSDK/testlib/` so the fix cannot silently regress. The testlib drives
`WorkerThread` without Logic 2, feeding edges directly from code.

```cpp
#include "TestInstance.h"
#include "MockChannelData.h"

// Build a synthetic PING frame:
// preamble (1111) + SOF (1001) + FT=0000 + TAG=0000 + EOF=0110 + postamble (1111)
MockChannelData clk, d0;
// push alternating clock edges and the corresponding D0 bit values...

AnalyzerTest::Instance inst;
inst.SetChannelData( clk, d0 );
inst.RunAnalyzerWorker();

auto& results = inst.GetResults();
assert( results[0].mType  == FSI_RESULT_PREAMBLE );
assert( results[1].mType  == FSI_RESULT_FRAME_TYPE );
assert( results[1].mData1 == FSI_FRAME_TYPE_PING );
assert( results[2].mType  == FSI_RESULT_TAG );
assert( results[3].mType  == FSI_RESULT_EOF );
```

See `AnalyzerSDK/testlib/TestInstance.h` and `MockChannelData.h` for the full
API. Wire the test target into `CMakeLists.txt` using the pattern in
`AnalyzerSDK/testlib/CMakeLists.txt`.

| What you want to change | File | Function / location |
|---|---|---|
|Preamble / SOF detection|`src/FSIAnalyzer.cpp`|`SyncPreamble()`|
|Bit collection (1-lane and 2-lane)|`src/FSIAnalyzer.cpp`|`CollectBits()`|
|CRC algorithm / lookup table|`src/FSIAnalyzer.cpp`|`kFsiCrcTable`, `ComputeCRC()`|
|Frame type → word count mapping|`src/FSIAnalyzer.cpp`|`DataWordCount()`|
|Frame type name strings|`src/FSIAnalyzer.cpp`|`FrameTypeName()`|
|Main decode loop|`src/FSIAnalyzer.cpp`|`WorkerThread()`|
|Logic 2 settings UI|`src/FSIAnalyzerSettings.cpp`|Constructor|
|Settings serialization|`src/FSIAnalyzerSettings.cpp`|`SaveSettings()`, `LoadSettings()`|
|Bubble text rendering|`src/FSIAnalyzerResults.cpp`|`GenerateBubbleText()`|
|Tabular text rendering|`src/FSIAnalyzerResults.cpp`|`GenerateFrameTabularText()`|
|CSV export|`src/FSIAnalyzerResults.cpp`|`GenerateExportFile()`|

---

## Further reading

| Document | What it covers |
|---|---|
|`state_machine.md`|Step-by-step preamble detection, ring buffer math, capture scenarios, flush sequence, full frame format and bit-level wire diagrams|
|`user_guide.md`|End-user installation, settings reference, waveform reading, CSV export, troubleshooting|
|`FSIFrame.pdf`|TI FSI TRM extract — authoritative protocol specification|

---

## Known limitations and open work

- **No automated tests.** `AnalyzerSDK/testlib/` is present but not wired to
  any test target. `SyncPreamble`, `CollectBits`, and `ComputeCRC` are the
  highest-value starting points.
- **`GenerateSimulationData()` is a stub** — returns 0, provides no synthetic
  waveform for offline testing inside Logic 2.
- **N-word count must be set manually.** The word count for `DATA(Nw)` frames
  cannot be determined from the wire — the UI setting must match
  `FSI_TX_FRAME_CTRL.N_WORDS` in firmware exactly.
- **Short inter-frame gaps may cause missed frames.** `SyncPreamble` requires
  ≥ 5 consecutive HIGH bits before committing to a frame start. Very tight
  back-to-back frame timing may result in frames being skipped.
