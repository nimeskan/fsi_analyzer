# TI FSI Analyzer — Developer Guide

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
                                                       emits FSI_RESULT_PREAMBLE
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
                         CollectBits(4, …, false)    — Postamble (consumed silently)
                         CommitResults()
```

Each field calls `mResults->AddFrame()` and `mResults->AddFrameV2()` immediately
after collection, not at the end of the frame.

### FSI frame structure (TRM Table 31-4 / §31.3.4.1)

```
Idle        : CLK=HIGH, D0=HIGH, D1=HIGH — no clock edges
Preamble    : 4 clock edges, data HIGH  (= 1111)
SOF         : 4 bits = 1001             — detected by SyncPreamble, no bubble
Frame Type  : 4 bits  [control field]
[data frames only]
  User Data : 8 bits  [interleaved in 2-lane]
  Data Words: N×16b   [interleaved in 2-lane]
  CRC       : 8 bits  [interleaved in 2-lane]
Frame Tag   : 4 bits  [control field]
EOF         : 4 bits = 0110  [control field]
Postamble   : 4 clock edges, data HIGH  (= 1111)  — consumed silently
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
|`FSI_RESULT_PREAMBLE`  |0x00|Preamble + SOF detected — bubble spans preamble[0] to SOF[0]|
|`FSI_RESULT_FRAME_TYPE`|0x01|4-bit frame type field|
|`FSI_RESULT_TAG`       |0x02|4-bit frame tag field|
|`FSI_RESULT_USERDATA`  |0x03|8-bit user data field (data frames only)|
|`FSI_RESULT_DATA_WORD` |0x04|Each 16-bit data word (`mData2` = zero-based word index)|
|`FSI_RESULT_CRC`       |0x05|8-bit CRC (`mFlags` bit 0: 1=OK, 0=FAIL)|
|`FSI_RESULT_EOF`       |0x06|EOF pattern (0110) validated|
|`FSI_RESULT_ERROR`     |0xFF|Bad EOF pattern — `mData1` holds the received value|

### `mData1`, `mData2`, and `mFlags` usage

| Result type | `mData1` | `mData2` | `mFlags` |
|---|---|---|---|
|`FSI_RESULT_PREAMBLE`  | 0 | 0 | 0 |
|`FSI_RESULT_FRAME_TYPE`| frame type code (0x0–0xF) | 0 | 0 |
|`FSI_RESULT_TAG`       | tag value (0x0–0xF) | 0 | 0 |
|`FSI_RESULT_USERDATA`  | user data byte | 0 | 0 |
|`FSI_RESULT_DATA_WORD` | 16-bit word value | zero-based word index | 0 |
|`FSI_RESULT_CRC`       | received CRC byte | computed (expected) CRC byte | bit 0: 1=match |
|`FSI_RESULT_EOF`       | 0x6 | 0 | 0 |
|`FSI_RESULT_ERROR`     | received EOF value | 0 | 0 |

### Preamble detection and bubble boundaries

`SyncPreamble` scans for ≥ 5 consecutive HIGH bits on D0 (4 preamble bits +
SOF[0]), then reads ahead for SOF[1,2,3] = 0,0,1. A 5-slot ring buffer of the
most-recent HIGH sample positions allows the preamble bubble to be placed
precisely:

- **Bubble start** (`pre_start`): `s_ring[r % 5]` — the oldest slot, always
  the sample of preamble bit 1.
- **Bubble end** (`pre_end`): `s_ring[(r-1+5) % 5]` — the most-recent slot,
  always the sample of SOF[0].
- **SOF[1,2,3]** are consumed during the look-ahead check and produce no bubble.
  They appear as unlabeled bits between the PRE and FT bubbles on the waveform.

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
   `PRE → FT → [UD → Data words → CRC] → TAG → EOF`.
4. Note that SOF[1,2,3] produce no bubble — a small unlabeled gap between
   PRE and FT is expected and correct.
5. Check that CRC bubbles show **OK** on valid data frames.
6. Use **Analyzers → Export** to produce a CSV and compare field values
   against the firmware's transmitted data.

---

## Debugging

### Use the CSV export as a ground truth

**Analyzers → Export → Export as CSV** produces one row per decoded field with
start and end sample numbers. Cross-check against what the firmware is actually
sending:

- Does every frame contain the expected field sequence (PRE, FT, UD, Data words, CRC, TAG, EOF)?
- Do `Type` values match what firmware is sending?
- Is `CRC OK` or `FAIL`? If FAIL, compare the `Value` column (received CRC byte)
  against the CRC computed from the frame's User Data and Data Words.
- Are `Start Sample` and `End Sample` consistent with your clock frequency?
  Divide the sample delta by your capture sample rate — it should equal one
  bit period (or half a bit period for 2-lane data fields).

The CSV is the fastest way to see exactly where the decoder diverges from
expectation before touching any code.

---

### Add file logging to the plugin

Logic 2 does not expose a console for plugin output. The plugin can write to a
log file instead. Insert logging around the suspect code path in
`src/FSIAnalyzer.cpp`:

```cpp
// Temporary — remove before release
#include <cstdio>

// Example: trace SyncPreamble's ring buffer state on every edge
FILE* dbg = fopen( "/tmp/fsi_debug.log", "a" );
fprintf( dbg, "high_count=%u  r=%u  sample=%llu  b0=%d\n",
         high_count, r, (unsigned long long)s, (int)b0 );
fclose( dbg );
```

Useful values to log depending on where the failure is:

| Suspect area | Values to log |
|---|---|
| Preamble not detected | `high_count`, `r`, `s` (sample number), `b0` on every edge |
| Wrong preamble bubble boundaries | `pre_start`, `pre_end`, `s_ring[0..4]` at commit |
| Wrong field value | `value` after each `CollectBits` call, `start_sample`, `end_sample` |
| CRC mismatch | Each byte pushed into `crc_buf`, `computed_crc`, `crc_val` |
| Wrong word count | `num_words`, `frame_type`, `mNWordCount` at the start of the data block |

Clear the log before each capture (`rm /tmp/fsi_debug.log`) so output stays
manageable. On Windows use a path like `C:\Temp\fsi_debug.log`. Rebuild the
plugin and restart Logic 2 after every code change.

---

### Capture scenario edge cases

If the **first frame is always missed**, the cause is almost always one of two
scenario-specific issues in `SyncPreamble` / `WorkerThread`:

**Scenario 1 — CLK starts LOW (MCU boots during capture):**

The MCU boot produces one rising CLK edge. If D0 is LOW at that edge,
`SyncPreamble` resets (`high_count=0; r=0`) — this is correct. The preamble
that follows then builds `high_count` from zero. If the first frame is still
missed, log `high_count` and `r` on every edge and confirm that exactly 5
HIGHs (P1, P2, P3, P4, SOF[0]) accumulate before SOF[1] arrives LOW.

**Scenario 2 — CLK starts HIGH (MCU already running when capture starts):**

The cursor sits blocked in `AdvanceToNextEdge` until the first falling clock
edge — which is preamble bit P1. If the first frame is missed, check whether
a clock pre-advance has been accidentally introduced at the top of
`WorkerThread`. There must be **no** call to `AdvanceToNextEdge` or
`AdvanceToNextClockEdge` before the `while(true)` loop. If one exists,
it consumes P1 before `SyncPreamble` ever reads it, leaving only P2–SOF[0]
(4 HIGHs), which is one short of the required 5.

See `state_machine.md` — "Capture scenarios" section — for complete
edge-by-edge traces and ring buffer state for both scenarios.

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
