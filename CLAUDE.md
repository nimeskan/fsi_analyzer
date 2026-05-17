# FSI Analyzer

A Saleae Logic 2 Low-Level Analyzer (LLA) plugin that decodes Texas Instruments
Fast Serial Interface (FSI) frames. Built as a C++14 shared library loaded by
Logic 2 at runtime.

---

## Documentation

| File | What it covers |
|---|---|
| `user_guide.md` | Installing the plugin, settings, reading the waveform, exporting CSV, troubleshooting |
| `developer_guide.md` | Architecture, build instructions, deploying to Logic 2, testing approach |
| `state_machine.md` | Step-by-step packet detection state machine, frame formats, bit-level wire diagrams, capture scenario analysis |
| `FSIFrame.pdf` | TI FSI TRM extract (source reference for the protocol) |

---

## Repository layout

```
fsi_analyzer/
├── src/                        — plugin source code
│   ├── FSIAnalyzer.h / .cpp    — main analyzer (WorkerThread, preamble sync, CRC)
│   ├── FSIAnalyzerSettings.h / .cpp  — Logic 2 settings UI and serialization
│   └── FSIAnalyzerResults.h / .cpp   — bubble text, tabular text, CSV export
├── AnalyzerSDK/                — bundled Saleae Analyzer SDK (pre-built, do not modify)
│   ├── include/                — SDK headers
│   ├── lib_x86_64/             — Linux x86-64 and Windows x64 SDK libraries
│   ├── lib_arm64/              — macOS arm64 SDK libraries
│   └── testlib/                — SDK unit-test harness (not yet wired up)
├── final_release/              — latest built plugin binaries
│   ├── FSIAnalyzer.so          — Linux x86-64
│   └── FSIAnalyzer.dll         — Windows x64
├── build/                      — CMake build output (not tracked)
├── SampleAnalyzer/             — upstream Saleae example project (reference only)
├── CMakeLists.txt              — build file
├── CLAUDE.md                   — this file
├── user_guide.md
├── developer_guide.md
├── state_machine.md
└── FSIFrame.pdf
```

---

## Quick start

```bash
mkdir build && cd build
cmake ..
cmake --build .
```

Output: `build/Analyzers/FSIAnalyzer.so` (Linux) or `.dylib` / `.dll` on other platforms.

See `developer_guide.md` for full build and deployment instructions.
