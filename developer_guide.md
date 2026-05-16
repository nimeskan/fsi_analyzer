# TI FSI Analyzer — Developer Guide

## Overview

This is a Saleae Logic 2 Low-Level Analyzer (LLA) plugin that decodes Texas
Instruments Fast Serial Interface (FSI) frames. It is written in C++14 and
built as a shared library loaded by Logic 2 at runtime.

-----

## Repository layout

```
fsi_analyzer/
├── CMakeLists.txt                  — top-level build file
├── AnalyzerSDK/                    — bundled Saleae Analyzer SDK (pre-built)
│   ├── include/                    — SDK headers
│   │   ├── Analyzer.h              — Analyzer2 base class, UseFrameV2()
│   │   ├── AnalyzerChannelData.h   — AnalyzerChannelData (channel reads)
│   │   ├── AnalyzerResults.h       — Frame, FrameV2, AnalyzerResults base
│   │   ├── AnalyzerSettings.h      — AnalyzerSettings base, SimpleArchive
│   │   ├── AnalyzerSettingInterface.h — UI widget interfaces
│   │   ├── AnalyzerHelpers.h       — GetNumberString(), etc.
│   │   └── ...
│   ├── lib_x86_64/                 — pre-built SDK library (Linux x86-64)
│   │   └── libAnalyzer.so
│   ├── lib_arm64/                  — pre-built SDK library (macOS arm64)
│   │   └── libAnalyzer.dylib
│   └── testlib/                    — SDK unit-test harness (MockChannelData, etc.)
├── SampleAnalyzer/                 — upstream Saleae example project (reference)
├── src/
│   ├── FSIAnalyzer.h / .cpp        — main analyzer: WorkerThread, bit collection,
│   │                                 preamble sync, CRC, frame emission
│   ├── FSIAnalyzerSettings.h / .cpp — Logic 2 settings UI and serialization
│   └── FSIAnalyzerResults.h / .cpp  — bubble text, tabular text, CSV export
├── build/                          — CMake build output (not tracked by default)
│   └── Analyzers/
│       └── FSIAnalyzer.so          — the plugin loaded by Logic 2
└── initial_deprecated_plan.md      — original design document (superseded)
```

-----

## Architecture

The plugin follows the Saleae LLA pattern: three classes derived from SDK
base classes, plus four C-linkage entry points.

```
Analyzer2  ←  FSIAnalyzer          (WorkerThread: parse bits → emit frames)
AnalyzerSettings ← FSIAnalyzerSettings  (UI: channel pickers, dropdowns)
AnalyzerResults  ← FSIAnalyzerResults   (render frames as text / CSV)
```

### Key call flow

```
Logic 2 loads .so → CreateAnalyzer() → FSIAnalyzer()
                  → SetupResults()
                  → WorkerThread() [runs on background thread]
                       SyncPreamble()
                       CollectBits(16) → header: frame_type, tag, user_data
                       CollectBits(16) × N → data words
                       CollectBits(8)  → CRC, verified against ComputeCRC()
                       CollectBits(4)  → EOF pattern (0x9)
                       mResults->AddFrame() + AddFrameV2() per field
                       mResults->CommitResults()
```

### Frame result types (defined in `FSIAnalyzer.h`)

| Constant | Value | Emitted for |
|---|---|---|
|`FSI_RESULT_PREAMBLE` |0x00|Flush+SOF preamble detected|
|`FSI_RESULT_FRAME_TYPE`|0x01|4-bit frame type field|
|`FSI_RESULT_TAG` |0x02|4-bit tag field|
|`FSI_RESULT_USERDATA` |0x03|8-bit user data field|
|`FSI_RESULT_DATA_WORD`|0x04|Each 16-bit data word (`mData2` = word index)|
|`FSI_RESULT_CRC` |0x05|8-bit CRC (`mFlags & 0x01` = CRC OK)|
|`FSI_RESULT_EOF` |0x06|EOF pattern validated|
|`FSI_RESULT_ERROR` |0xFF|Bad EOF or framing error|

### `mFlags` usage

| Frame type | Bit | Meaning |
|---|---|---|
|`FSI_RESULT_CRC`|bit 0 (0x01)|1 = CRC matched, 0 = CRC failed|

### 2-lane interleaving (`CollectBits`)

In 2-lane mode, each clock edge delivers two logical bits:
- TXD0 → even-indexed bits (positions 0, 2, 4, …)
- TXD1 → odd-indexed bits (positions 1, 3, 5, …)

`ceil(count/2)` clock edges are consumed per `CollectBits(count)` call.
Both bits are shifted into `value` MSB-first in interleaved order.

### CRC

FSI uses CRC-8, polynomial `x^8 + x^2 + x + 1` (0x07), seed 0x00,
no final XOR. The lookup table `kFsiCrcTable[256]` in `FSIAnalyzer.cpp`
implements this. The CRC covers the User Data byte only from the header
(Frame Type and Tag are excluded), followed by each data word little-endian
(LSB first, then MSB). `mData2` on the CRC frame holds the computed expected
value for debugging.

-----

## Build prerequisites

| Tool | Minimum version |
|---|---|
|CMake|3.13|
|GCC or Clang (Linux)|any C++14-capable|
|MSVC (Windows)|2019+ x64|
|Xcode / clang (macOS)|any C++14-capable|

The `AnalyzerSDK/` directory is bundled in the repository — no separate
download is required.

-----

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

CMake auto-detects the SDK library path: it prefers `AnalyzerSDK/lib_x86_64/`
and falls back to `AnalyzerSDK/lib/` if that directory is absent.

### macOS

```bash
mkdir build && cd build
cmake ..
cmake --build .
xattr -d com.apple.quarantine build/Analyzers/FSIAnalyzer.dylib
```

Output: `build/Analyzers/FSIAnalyzer.dylib`

> **Note:** The bundled macOS library is in `AnalyzerSDK/lib_arm64/`. If you
> are on Intel macOS, you may need to update `SALEAE_LIB` in `CMakeLists.txt`
> to point at an x86-64 macOS build of `libAnalyzer.dylib` from the upstream
> Saleae SDK release.

### Windows (x64 Developer Command Prompt)

```bat
mkdir build && cd build
cmake .. -A x64
cmake --build . --config Release
```

Output: `build\Analyzers\Release\FSIAnalyzer.dll`

> **Note:** The bundled `AnalyzerSDK/lib_x86_64/Analyzer.lib` and
> `Analyzer.dll` are used. Update `SALEAE_LIB` / `SALEAE_DLL` in
> `CMakeLists.txt` if targeting ARM64 Windows.

### Important CMake flags

`add_definitions(-DLOGIC2)` is set unconditionally in `CMakeLists.txt`.
This preprocessor define unlocks `FrameV2`, `AddFrameV2`, and `UseFrameV2()`
in `AnalyzerResults.h`. Removing it will cause a build failure.

-----

## Deploying to Logic 2

1. Build the plugin (see above).
2. Open **Logic 2**.
3. Go to **Edit → Settings** and scroll to **Custom Low Level Analyzers**.
4. Click **+** and add the path to `build/Analyzers/`.
5. **Restart Logic 2** — the directory is only scanned at startup.
6. In a capture session, click **Analyzers → +** and search for
   **TI FSI**.

To update after a recompile, Logic 2 must be restarted. There is no hot-reload
mechanism for LLA plugins.

-----

## Testing

The Saleae SDK ships a unit-test harness in `AnalyzerSDK/testlib/`. Key types:

| Class | Purpose |
|---|---|
|`AnalyzerTest::Instance`|Loads the analyzer and drives `WorkerThread`|
|`AnalyzerTest::MockChannelData`|Feeds synthetic edge data to channel inputs|
|`AnalyzerTest::MockResults`|Captures emitted `Frame` and `FrameV2` objects|

No automated tests are currently wired up in this project. To add them:

1. Create a `tests/` directory alongside `src/`.
2. Add a new `CMakeLists.txt` target that links against
   `AnalyzerSDK/testlib/` sources and your analyzer objects.
3. In test code, construct an `AnalyzerTest::Instance`, push synthetic clock
   and data edges via `MockChannelData`, call `RunAnalyzerWorker()`, then
   assert on `GetResults()`.

### Manual verification workflow

1. Connect TXCLK and TXD0 (and TXD1 if 2-lane) to a Logic device running
   at ≥ 4× your FSI clock frequency (e.g. 200 MS/s for a 50 MHz FSI clock;
   a Logic 4 at 12 MS/s suffices for FSI clocks up to ~3 MHz).
2. Load the plugin and add the analyzer to the capture.
3. Inspect bubble labels: each FSI field should appear as a labelled segment.
4. Check that CRC bubbles show **OK** on valid frames.
5. Use **Analyzers → Export** to produce a CSV and compare field values
   against the firmware's transmitted data.

-----

## Key source locations

| What you want to change | File | Where |
|---|---|---|
|Preamble / SOF detection logic|`src/FSIAnalyzer.cpp`|`SyncPreamble()`|
|Bit collection (1-lane and 2-lane)|`src/FSIAnalyzer.cpp`|`CollectBits()`|
|CRC algorithm / lookup table|`src/FSIAnalyzer.cpp`|`kFsiCrcTable`, `ComputeCRC()`|
|Frame type → word count mapping|`src/FSIAnalyzer.cpp`|`DataWordCount()`|
|Frame type name strings|`src/FSIAnalyzer.cpp`|`FrameTypeName()`|
|Logic 2 settings UI|`src/FSIAnalyzerSettings.cpp`|Constructor|
|Settings serialization|`src/FSIAnalyzerSettings.cpp`|`SaveSettings()`, `LoadSettings()`|
|Bubble text rendering|`src/FSIAnalyzerResults.cpp`|`GenerateBubbleText()`|
|Tabular text rendering|`src/FSIAnalyzerResults.cpp`|`GenerateFrameTabularText()`|
|CSV export|`src/FSIAnalyzerResults.cpp`|`GenerateExportFile()`|

-----

## Known limitations and open work

- **No automated tests.** `AnalyzerSDK/testlib/` is present but not wired to
  any test target. Adding coverage for `CollectBits`, `ComputeCRC`, and
  `SyncPreamble` is the highest-value starting point.
- **`GenerateSimulationData()` is a stub** — returns 0, provides no synthetic
  waveform for offline testing inside Logic 2.
- **N-word count** cannot be inferred from the wire. The UI dropdown must be
  set to match `FSI_TX_FRAME_CTRL.N_WORDS` in firmware.
