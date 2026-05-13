# TI FSI (C2000) — Saleae Logic 2 Low-Level Analyzer

Decodes Texas Instruments **Fast Serial Interface (FSI)** frames from C2000
MCUs (F280049, F2838x, F28003x, etc.) in Saleae **Logic 2**.

-----

## What it decodes

|Field     |Description                             |
|----------|----------------------------------------|
|Preamble  |Flush pattern + Start-of-Frame detection|
|Frame Type|PING / ERROR / DATA(1w/2w/4w/6w/Nw)     |
|Tag       |4-bit user tag (0–15)                   |
|User Data |8-bit extra user field                  |
|Data Words|16-bit words (index shown on bubble)    |
|CRC       |8-bit hardware CRC — shown OK or **BAD**|
|EOF       |End-of-frame pattern validation         |

-----

## Hardware setup

|FSI signal|Saleae input                                      |
|----------|--------------------------------------------------|
|TXCLK     |Any channel (set as "TXCLK")                      |
|TXD0      |Any channel (set as "TXD0/RXD0")                  |
|TXD1      |Any channel (set as "TXD1/RXD1", 2-lane mode only)|

**Important:** FSI runs up to 50 MHz clock (DDR → 100 Mbps on 1 lane).
Use **Logic Pro 8** or **Logic Pro 16** at ≥ 200 MS/s.
Logic 4 does not have sufficient bandwidth.

If your FSI uses LVDS or isolation transceivers, probe the **CMOS side**
after the transceiver — Saleae inputs are single-ended.

-----

## Build instructions

### Prerequisites

- CMake ≥ 3.13
- C++14 compiler:
  - **Windows**: MSVC 2019+ (x64)
  - **Linux**: GCC or Clang
  - **macOS**: Xcode / clang

### 1. Get the source

The `AnalyzerSDK/` directory is already bundled in this repository.
No separate SDK download is needed — just clone this repo:

```bash
git clone https://github.com/nimeskan/fsi_analyzer
```

The project directory structure is:

```
fsi_analyzer/
├── AnalyzerSDK/          ← bundled Saleae SDK
├── CMakeLists.txt
└── src/
    ├── FSIAnalyzer.h
    ├── FSIAnalyzer.cpp
    ├── FSIAnalyzerSettings.h
    ├── FSIAnalyzerSettings.cpp
    ├── FSIAnalyzerResults.h
    └── FSIAnalyzerResults.cpp
```

### 2. Build

**Linux / macOS:**

```bash
mkdir build && cd build
cmake ..
cmake --build .
# Output: build/Analyzers/FSIAnalyzer.so  (Linux)
#         build/Analyzers/FSIAnalyzer.dylib (macOS)
```

**Windows (x64 Developer Command Prompt):**

```bat
mkdir build && cd build
cmake .. -A x64
cmake --build . --config Release
:: Output: build\Analyzers\Release\FSIAnalyzer.dll
```

### 3. macOS quarantine removal

```bash
xattr -d com.apple.quarantine build/Analyzers/FSIAnalyzer.dylib
```

-----

## Loading into Logic 2

1. Open **Logic 2**
1. Go to **Preferences → Custom Low Level Analyzers**
1. Add the path to the `build/Analyzers/` directory
1. **Restart Logic 2**
1. In a capture, click **Analyzers → +** and search for **TI FSI (C2000)**
1. Assign channels: TXCLK, TXD0, and optionally TXD1
1. Enable **2-Lane Mode** if using both TXD0 and TXD1

-----

## Analyzer settings

|Setting            |Description                                                                                                                 |
|-------------------|----------------------------------------------------------------------------------------------------------------------------|
|TXCLK/RXCLK        |Clock channel                                                                                                               |
|TXD0/RXD0          |Data lane 0 (required)                                                                                                      |
|TXD1/RXD1          |Data lane 1 (optional, 2-lane mode only)                                                                                    |
|2-Lane Mode        |Enable dual-lane DDR interleaved capture                                                                                    |
|N-Word Frame Count |Number of words in N-word frames (1–16). Must match `FSI_TX_FRAME_CTRL.N_WORDS` in firmware. No effect on fixed-size frames.|
|SPI-Compatible Mode|Enable when `FSI_TX_COMPAT_MODE` is set in firmware. Replaces flush+SOF with SPI CS assertion.                              |

### 2-Lane mode — how it works

In 2-lane mode the FSI peripheral transmits two bits per clock edge:

- **TXD0** carries even-indexed bits (bit positions 0, 2, 4 … counting from MSB)
- **TXD1** carries odd-indexed bits (bit positions 1, 3, 5 …)

Both bits are sampled on the **same** clock edge, giving double throughput.

-----

## Fixes applied (v2)

|#|Issue                                                                       |Fix                                                                             |
|-|----------------------------------------------------------------------------|--------------------------------------------------------------------------------|
|1|**2-lane interleaving wrong** — TXD1 ignored entirely                       |`CollectBits()` now reads D0+D1 per edge and interleaves even/odd bits correctly|
|2|**N-word count hardcoded to 16** — wrong for most applications              |Added UI dropdown (1–16). Value passed to `DataWordCount()` at runtime          |
|3|**CRC buffer stack overflow** — `U8 crc_buf[34]` overflows on any off-by-one|Changed to `std::vector<U8>` — grows dynamically, no fixed limit                |
|4|**`UseFramesV2()` typo** — symbol does not exist in the SDK                 |Corrected to `UseFrameV2()`                                                     |
|5|**`AnalyzerChannelData` incomplete type** — `Analyzer.h` only forward-declares it|Added `#include <AnalyzerChannelData.h>` in `FSIAnalyzer.cpp`              |
|6|**`FrameTypeName` used before definition** — `WorkerThread()` precedes it in TU|Added forward declaration at top of `FSIAnalyzer.cpp`                       |
|7|**Linux lib path wrong** — SDK ships `lib_x86_64/` not `lib/`              |`CMakeLists.txt` auto-detects `lib_x86_64/` with fallback to `lib/`            |

Bonus: `SaveSettings()` now stores to `mSavedSettings` (a private member `std::string`) instead of the SDK-inherited `mSettings` variable, fixing a subtle name-shadowing bug.

-----

## Source files

### `CMakeLists.txt`

```cmake
cmake_minimum_required(VERSION 3.13)
project(FSIAnalyzer)

set(ANALYZER_SDK_PATH "${CMAKE_CURRENT_SOURCE_DIR}/AnalyzerSDK")

if(NOT EXISTS "${ANALYZER_SDK_PATH}/include/Analyzer.h")
    message(FATAL_ERROR
        "AnalyzerSDK not found at ${ANALYZER_SDK_PATH}.\n"
        "Clone with: git clone --recurse-submodules https://github.com/saleae/SampleAnalyzer\n"
        "Then place AnalyzerSDK alongside this CMakeLists.txt.")
endif()

if(APPLE)
    set(SALEAE_LIB "${ANALYZER_SDK_PATH}/lib/libAnalyzer.dylib")
elseif(WIN32)
    set(SALEAE_LIB "${ANALYZER_SDK_PATH}/lib/Analyzer.lib")
    set(SALEAE_DLL "${ANALYZER_SDK_PATH}/lib/Analyzer.dll")
else()
    # SDK ships arch-specific lib dirs
    if(EXISTS "${ANALYZER_SDK_PATH}/lib_x86_64/libAnalyzer.so")
        set(SALEAE_LIB "${ANALYZER_SDK_PATH}/lib_x86_64/libAnalyzer.so")
    else()
        set(SALEAE_LIB "${ANALYZER_SDK_PATH}/lib/libAnalyzer.so")
    endif()
endif()

add_definitions(-DLOGIC2)

add_library(FSIAnalyzer SHARED
    src/FSIAnalyzer.cpp
    src/FSIAnalyzerSettings.cpp
    src/FSIAnalyzerResults.cpp
)

target_include_directories(FSIAnalyzer PRIVATE
    "${ANALYZER_SDK_PATH}/include"
    src/
)

target_link_libraries(FSIAnalyzer PRIVATE "${SALEAE_LIB}")

set_target_properties(FSIAnalyzer PROPERTIES
    CXX_STANDARD 14
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Analyzers"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Analyzers"
)

if(WIN32)
    add_custom_command(TARGET FSIAnalyzer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${SALEAE_DLL}"
            "$<TARGET_FILE_DIR:FSIAnalyzer>"
    )
endif()
```

-----

### `src/FSIAnalyzerSettings.h`

```cpp
#pragma once
#include <AnalyzerSettings.h>
#include <AnalyzerTypes.h>

class FSIAnalyzerSettings : public AnalyzerSettings
{
public:
    FSIAnalyzerSettings();
    virtual ~FSIAnalyzerSettings();

    virtual bool SetSettingsFromInterfaces();
    virtual void LoadSettings( const char* settings );
    virtual const char* SaveSettings();

    void UpdateInterfacesFromSettings();

    // Channels
    Channel mClockChannel;
    Channel mDataChannel0;      // TXD0 / RXD0
    Channel mDataChannel1;      // TXD1 / RXD1 (optional, 2-lane)

    // Protocol options
    bool    mTwoLane;           // true = 2-lane DDR interleaved mode
    U32     mNWordCount;        // word count for N-word frames (1-16)
    bool    mSpiCompatMode;     // true = SPI-compatible preamble (CS-based)

protected:
    std::unique_ptr<AnalyzerSettingInterfaceChannel>    mClockChannelInterface;
    std::unique_ptr<AnalyzerSettingInterfaceChannel>    mDataChannel0Interface;
    std::unique_ptr<AnalyzerSettingInterfaceChannel>    mDataChannel1Interface;
    std::unique_ptr<AnalyzerSettingInterfaceBool>       mTwoLaneInterface;
    std::unique_ptr<AnalyzerSettingInterfaceNumberList> mNWordCountInterface;
    std::unique_ptr<AnalyzerSettingInterfaceBool>       mSpiCompatModeInterface;

private:
    std::string mSavedSettings;   // owns the string returned by SaveSettings()
};
```

-----

### `src/FSIAnalyzerSettings.cpp`

```cpp
#include "FSIAnalyzerSettings.h"
#include <AnalyzerHelpers.h>

FSIAnalyzerSettings::FSIAnalyzerSettings()
    : mClockChannel( UNDEFINED_CHANNEL ),
      mDataChannel0( UNDEFINED_CHANNEL ),
      mDataChannel1( UNDEFINED_CHANNEL ),
      mTwoLane( false ),
      mNWordCount( 16 ),
      mSpiCompatMode( false )
{
    mClockChannelInterface.reset( new AnalyzerSettingInterfaceChannel() );
    mClockChannelInterface->SetTitleAndTooltip( "TXCLK / RXCLK", "FSI clock line" );
    mClockChannelInterface->SetChannel( mClockChannel );

    mDataChannel0Interface.reset( new AnalyzerSettingInterfaceChannel() );
    mDataChannel0Interface->SetTitleAndTooltip( "TXD0 / RXD0",
        "FSI data lane 0 - always required" );
    mDataChannel0Interface->SetChannel( mDataChannel0 );

    mDataChannel1Interface.reset( new AnalyzerSettingInterfaceChannel() );
    mDataChannel1Interface->SetTitleAndTooltip( "TXD1 / RXD1 (2-lane only)",
        "FSI data lane 1 - only needed in 2-lane mode" );
    mDataChannel1Interface->SetChannel( mDataChannel1 );
    mDataChannel1Interface->SetSelectionOfNoneIsAllowed( true );

    mTwoLaneInterface.reset( new AnalyzerSettingInterfaceBool() );
    mTwoLaneInterface->SetTitleAndTooltip( "2-Lane Mode",
        "Enable dual-lane capture (TXD0 + TXD1). "
        "Even-numbered bits arrive on TXD0, odd-numbered bits on TXD1." );
    mTwoLaneInterface->SetValue( mTwoLane );

    mNWordCountInterface.reset( new AnalyzerSettingInterfaceNumberList() );
    mNWordCountInterface->SetTitleAndTooltip( "N-Word Frame Count (type 0x6)",
        "Number of 16-bit data words in N-word frames. "
        "Must match FSI_TX_FRAME_CTRL.N_WORDS in your firmware. "
        "Has no effect on fixed-size frames (PING/ERROR/DATA 1-6w)." );
    for( U32 n = 1; n <= 16; n++ )
    {
        char label[8];
        snprintf( label, sizeof(label), "%u", n );
        mNWordCountInterface->AddNumber( (double)n, label,
            n == 16 ? "Maximum (default)" : "" );
    }
    mNWordCountInterface->SetNumber( (double)mNWordCount );

    mSpiCompatModeInterface.reset( new AnalyzerSettingInterfaceBool() );
    mSpiCompatModeInterface->SetTitleAndTooltip( "SPI-Compatible Mode",
        "Enable when FSI_TX_COMPAT_MODE is set in firmware. "
        "Replaces flush+SOF preamble with SPI chip-select assertion. "
        "Frame structure (header, data, CRC, EOF) is identical." );
    mSpiCompatModeInterface->SetValue( mSpiCompatMode );

    AddInterface( mClockChannelInterface.get() );
    AddInterface( mDataChannel0Interface.get() );
    AddInterface( mDataChannel1Interface.get() );
    AddInterface( mTwoLaneInterface.get() );
    AddInterface( mNWordCountInterface.get() );
    AddInterface( mSpiCompatModeInterface.get() );

    AddExportOption( 0, "Export as CSV" );
    AddExportExtension( 0, "CSV", "csv" );

    ClearChannels();
    AddChannel( mClockChannel,  "TXCLK",     false );
    AddChannel( mDataChannel0,  "TXD0/RXD0", false );
    AddChannel( mDataChannel1,  "TXD1/RXD1", false );
}

FSIAnalyzerSettings::~FSIAnalyzerSettings() {}

bool FSIAnalyzerSettings::SetSettingsFromInterfaces()
{
    mClockChannel  = mClockChannelInterface->GetChannel();
    mDataChannel0  = mDataChannel0Interface->GetChannel();
    mDataChannel1  = mDataChannel1Interface->GetChannel();
    mTwoLane       = mTwoLaneInterface->GetValue();
    mNWordCount    = (U32)mNWordCountInterface->GetNumber();
    mSpiCompatMode = mSpiCompatModeInterface->GetValue();

    if( mClockChannel == UNDEFINED_CHANNEL )
    {
        SetErrorText( "Please select the clock channel (TXCLK/RXCLK)." );
        return false;
    }
    if( mDataChannel0 == UNDEFINED_CHANNEL )
    {
        SetErrorText( "Please select the TXD0/RXD0 data channel." );
        return false;
    }
    if( mTwoLane && mDataChannel1 == UNDEFINED_CHANNEL )
    {
        SetErrorText( "2-lane mode is enabled but TXD1/RXD1 channel is not assigned." );
        return false;
    }
    if( mNWordCount < 1 || mNWordCount > 16 )
    {
        SetErrorText( "N-Word count must be between 1 and 16." );
        return false;
    }

    ClearChannels();
    AddChannel( mClockChannel,  "TXCLK",     true );
    AddChannel( mDataChannel0,  "TXD0/RXD0", true );
    if( mTwoLane )
        AddChannel( mDataChannel1, "TXD1/RXD1", true );

    return true;
}

void FSIAnalyzerSettings::UpdateInterfacesFromSettings()
{
    mClockChannelInterface->SetChannel( mClockChannel );
    mDataChannel0Interface->SetChannel( mDataChannel0 );
    mDataChannel1Interface->SetChannel( mDataChannel1 );
    mTwoLaneInterface->SetValue( mTwoLane );
    mNWordCountInterface->SetNumber( (double)mNWordCount );
    mSpiCompatModeInterface->SetValue( mSpiCompatMode );
}

void FSIAnalyzerSettings::LoadSettings( const char* settings )
{
    SimpleArchive text_archive;
    text_archive.SetString( settings );
    text_archive >> mClockChannel;
    text_archive >> mDataChannel0;
    text_archive >> mDataChannel1;
    text_archive >> mTwoLane;
    text_archive >> mNWordCount;
    text_archive >> mSpiCompatMode;

    ClearChannels();
    AddChannel( mClockChannel,  "TXCLK",     true );
    AddChannel( mDataChannel0,  "TXD0/RXD0", true );
    if( mTwoLane )
        AddChannel( mDataChannel1, "TXD1/RXD1", true );

    UpdateInterfacesFromSettings();
}

const char* FSIAnalyzerSettings::SaveSettings()
{
    SimpleArchive text_archive;
    text_archive << mClockChannel;
    text_archive << mDataChannel0;
    text_archive << mDataChannel1;
    text_archive << mTwoLane;
    text_archive << mNWordCount;
    text_archive << mSpiCompatMode;

    mSavedSettings = text_archive.GetString();
    return mSavedSettings.c_str();
}
```

-----

### `src/FSIAnalyzerResults.h`

```cpp
#pragma once
#include <AnalyzerResults.h>

class FSIAnalyzerSettings;
class FSIAnalyzer;

class FSIAnalyzerResults : public AnalyzerResults
{
public:
    FSIAnalyzerResults( FSIAnalyzer* analyzer, FSIAnalyzerSettings* settings );
    virtual ~FSIAnalyzerResults();

    virtual void GenerateBubbleText( U64 frame_index, Channel& channel,
                                     DisplayBase display_base );
    virtual void GenerateExportFile( const char* file, DisplayBase display_base,
                                     U32 export_type_user_id );
    virtual void GenerateFrameTabularText( U64 frame_index,
                                           DisplayBase display_base );
    virtual void GeneratePacketTabularText( U64 packet_id,
                                            DisplayBase display_base );
    virtual void GenerateTransactionTabularText( U64 transaction_id,
                                                 DisplayBase display_base );

protected:
    FSIAnalyzerSettings* mSettings;
    FSIAnalyzer*         mAnalyzer;
};
```

-----

### `src/FSIAnalyzerResults.cpp`

```cpp
#include "FSIAnalyzerResults.h"
#include "FSIAnalyzer.h"
#include "FSIAnalyzerSettings.h"
#include <AnalyzerHelpers.h>
#include <fstream>
#include <sstream>

// Defined in FSIAnalyzer.cpp — shared label helper
extern const char* FrameTypeName( U64 ft );

FSIAnalyzerResults::FSIAnalyzerResults( FSIAnalyzer* analyzer,
                                        FSIAnalyzerSettings* settings )
    : AnalyzerResults(), mSettings( settings ), mAnalyzer( analyzer )
{
}

FSIAnalyzerResults::~FSIAnalyzerResults() {}

void FSIAnalyzerResults::GenerateBubbleText( U64 frame_index,
                                              Channel& /*channel*/,
                                              DisplayBase display_base )
{
    ClearResultStrings();
    Frame frame = GetFrame( frame_index );

    char number_str[64];

    switch( frame.mType )
    {
    case FSI_RESULT_PREAMBLE:
        if( frame.mFlags & 0x02 )
        {
            AddResultString( "CS" );
            AddResultString( "SPI-Compat CS" );
        }
        else
        {
            AddResultString( "PRE" );
            AddResultString( "Preamble" );
        }
        break;

    case FSI_RESULT_FRAME_TYPE:
        AddResultString( "FT" );
        AddResultString( FrameTypeName( frame.mData1 ) );
        break;

    case FSI_RESULT_TAG:
        AnalyzerHelpers::GetNumberString( frame.mData1, display_base, 4, number_str, 64 );
        AddResultString( "TAG" );
        { std::string s = std::string("Tag: ") + number_str; AddResultString( s.c_str() ); }
        break;

    case FSI_RESULT_USERDATA:
        AnalyzerHelpers::GetNumberString( frame.mData1, display_base, 8, number_str, 64 );
        AddResultString( "UD" );
        { std::string s = std::string("UserData: ") + number_str; AddResultString( s.c_str() ); }
        break;

    case FSI_RESULT_DATA_WORD:
        AnalyzerHelpers::GetNumberString( frame.mData1, display_base, 16, number_str, 64 );
        {
            std::string short_s = std::string("D") + number_str;
            std::string long_s  = std::string("Data[") + std::to_string(frame.mData2) + "]: " + number_str;
            AddResultString( short_s.c_str() );
            AddResultString( long_s.c_str() );
        }
        break;

    case FSI_RESULT_CRC:
        AnalyzerHelpers::GetNumberString( frame.mData1, display_base, 8, number_str, 64 );
        {
            std::string s = std::string("CRC: ") + number_str;
            bool crc_ok = ( frame.mFlags & 0x01 ) != 0;
            if( !crc_ok ) s += " [BAD]";
            AddResultString( "CRC" );
            AddResultString( s.c_str() );
        }
        break;

    case FSI_RESULT_EOF:
        AddResultString( "EOF" );
        AddResultString( "End of Frame" );
        break;

    case FSI_RESULT_ERROR:
        AddResultString( "ERR" );
        AddResultString( "Framing Error" );
        break;
    }
}

void FSIAnalyzerResults::GenerateExportFile( const char* file,
                                              DisplayBase display_base,
                                              U32 /*export_type_user_id*/ )
{
    std::ofstream file_stream( file, std::ios::out );
    file_stream << "Frame Index,Type,Value,Word Index,CRC OK,Start Sample,End Sample\n";

    U64 num_frames = GetNumFrames();
    char number_str[64];

    for( U64 i = 0; i < num_frames; i++ )
    {
        Frame frame = GetFrame( i );
        AnalyzerHelpers::GetNumberString( frame.mData1, display_base, 16, number_str, 64 );

        file_stream << i << ",";

        switch( frame.mType )
        {
        case FSI_RESULT_PREAMBLE:   file_stream << "Preamble,,,,"; break;
        case FSI_RESULT_FRAME_TYPE: file_stream << "FrameType," << FrameTypeName( frame.mData1 ) << ",,,"; break;
        case FSI_RESULT_TAG:        file_stream << "Tag," << number_str << ",,,"; break;
        case FSI_RESULT_USERDATA:   file_stream << "UserData," << number_str << ",,,"; break;
        case FSI_RESULT_DATA_WORD:
            file_stream << "DataWord," << number_str << "," << frame.mData2 << ",,";
            break;
        case FSI_RESULT_CRC:
            file_stream << "CRC," << number_str << ",,"
                        << ( ( frame.mFlags & 0x01 ) ? "OK" : "FAIL" ) << ",";
            break;
        case FSI_RESULT_EOF:   file_stream << "EOF,,,,"; break;
        case FSI_RESULT_ERROR: file_stream << "Error,,,,"; break;
        default:               file_stream << "Unknown,,,,"; break;
        }

        file_stream << frame.mStartingSampleInclusive << ","
                    << frame.mEndingSampleInclusive << "\n";

        if( UpdateExportProgressAndCheckForCancel( i, num_frames ) )
            return;
    }

    file_stream.close();
}

void FSIAnalyzerResults::GenerateFrameTabularText( U64 frame_index,
                                                    DisplayBase display_base )
{
    ClearTabularText();
    Frame frame = GetFrame( frame_index );
    char number_str[64];

    switch( frame.mType )
    {
    case FSI_RESULT_PREAMBLE:
        AddTabularText( "Preamble" );
        break;
    case FSI_RESULT_FRAME_TYPE:
        AddTabularText( FrameTypeName( frame.mData1 ) );
        break;
    case FSI_RESULT_TAG:
        AnalyzerHelpers::GetNumberString( frame.mData1, display_base, 4, number_str, 64 );
        { std::string s = std::string("Tag=") + number_str; AddTabularText( s.c_str() ); }
        break;
    case FSI_RESULT_USERDATA:
        AnalyzerHelpers::GetNumberString( frame.mData1, display_base, 8, number_str, 64 );
        { std::string s = std::string("UserData=") + number_str; AddTabularText( s.c_str() ); }
        break;
    case FSI_RESULT_DATA_WORD:
        AnalyzerHelpers::GetNumberString( frame.mData1, display_base, 16, number_str, 64 );
        {
            std::string s = std::string("D[") + std::to_string(frame.mData2) + "]=" + number_str;
            AddTabularText( s.c_str() );
        }
        break;
    case FSI_RESULT_CRC:
        AnalyzerHelpers::GetNumberString( frame.mData1, display_base, 8, number_str, 64 );
        {
            std::string s = std::string("CRC=") + number_str +
                            ( ( frame.mFlags & 0x01 ) ? " OK" : " BAD" );
            AddTabularText( s.c_str() );
        }
        break;
    case FSI_RESULT_EOF:
        AddTabularText( "EOF" );
        break;
    case FSI_RESULT_ERROR:
        AddTabularText( "FRAMING ERROR" );
        break;
    }
}

void FSIAnalyzerResults::GeneratePacketTabularText( U64, DisplayBase ) { ClearTabularText(); }
void FSIAnalyzerResults::GenerateTransactionTabularText( U64, DisplayBase ) { ClearTabularText(); }
```

-----

### `src/FSIAnalyzer.h`

```cpp
#pragma once
#include <Analyzer.h>
#include <vector>
#include "FSIAnalyzerSettings.h"
#include "FSIAnalyzerResults.h"

// -----------------------------------------------------------------------
// FSI Frame Types (4-bit field)
// -----------------------------------------------------------------------
#define FSI_FRAME_TYPE_PING     0x0
#define FSI_FRAME_TYPE_ERROR    0x1
#define FSI_FRAME_TYPE_DATA1    0x2   // 1-word data
#define FSI_FRAME_TYPE_DATA2    0x3   // 2-word data
#define FSI_FRAME_TYPE_DATA4    0x4   // 4-word data
#define FSI_FRAME_TYPE_DATA6    0x5   // 6-word data
#define FSI_FRAME_TYPE_NWORD    0x6   // N-word (software configured)

// -----------------------------------------------------------------------
// Saleae Frame types emitted by this analyzer
// -----------------------------------------------------------------------
#define FSI_RESULT_PREAMBLE     0x00
#define FSI_RESULT_FRAME_TYPE   0x01
#define FSI_RESULT_TAG          0x02
#define FSI_RESULT_USERDATA     0x03
#define FSI_RESULT_DATA_WORD    0x04
#define FSI_RESULT_CRC          0x05
#define FSI_RESULT_EOF          0x06
#define FSI_RESULT_ERROR        0xFF

class FSIAnalyzer : public Analyzer2
{
public:
    FSIAnalyzer();
    virtual ~FSIAnalyzer();

    virtual void SetupResults();
    virtual void WorkerThread();

    virtual U32  GenerateSimulationData( U64 minimum_sample_index,
                                         U32 device_sample_rate,
                                         SimulationChannelDescriptor** simulation_channels );
    virtual U32  GetMinimumSampleRateHz();
    virtual const char* GetAnalyzerName() const;
    virtual bool NeedsRerun();

protected:
    // Advance to next DDR clock edge.
    // In 1-lane: lane0_bit is the bit on TXD0.
    // In 2-lane: lane0_bit = TXD0 (even bit), lane1_bit = TXD1 (odd bit).
    void AdvanceToNextClockEdge( BitState& lane0_bit, BitState& lane1_bit );

    // Collect 'count' logical bits into a value (MSB first).
    // 1-lane: reads 'count' DDR edges from TXD0 only.
    // 2-lane: reads ceil(count/2) edges; each edge delivers D0 (even) + D1 (odd).
    bool CollectBits( U32 count, U64& value,
                      U64& start_sample, U64& end_sample );

    // Returns the number of data words for a given frame type.
    // Uses mNWordCount for type 0x6.
    U32  DataWordCount( U8 frame_type ) const;

    // Detect flush+SOF preamble (normal FSI mode).
    bool SyncPreamble( U64& frame_start_sample );

    // Detect SPI-compatible frame start (CS assertion = TXD0 going LOW).
    bool SyncSpiCompat( U64& frame_start_sample );

    // Compute FSI CRC-8 over a byte vector.
    U8   ComputeCRC( const std::vector<U8>& data );

    FSIAnalyzerSettings mSettings;
    std::unique_ptr<FSIAnalyzerResults> mResults;
    AnalyzerChannelData* mClock;
    AnalyzerChannelData* mData0;
    AnalyzerChannelData* mData1;    // nullptr when 1-lane

    U32  mSampleRateHz;
    bool mTwoLane;
    U32  mNWordCount;
    bool mSpiCompatMode;

    U64      mLastClockSample;
    BitState mLastClockState;
};

extern "C" ANALYZER_EXPORT const char* __cdecl GetAnalyzerName();
extern "C" ANALYZER_EXPORT Analyzer*   __cdecl CreateAnalyzer();
extern "C" ANALYZER_EXPORT void        __cdecl DestroyAnalyzer( Analyzer* analyzer );
```

-----

### `src/FSIAnalyzer.cpp`

```cpp
#include "FSIAnalyzer.h"
#include "FSIAnalyzerResults.h"
#include "FSIAnalyzerSettings.h"
#include <AnalyzerHelpers.h>
#include <AnalyzerChannelData.h>
#include <vector>

const char* FrameTypeName( U64 ft );

// ============================================================================
//  FSI CRC-8
//  Polynomial: x^8 + x^6 + x^3 + x^2 + 1 (0x4D)
//  Seed = 0x00, no final XOR.
// ============================================================================
static const U8 kFsiCrcTable[256] = {
    0x00,0x07,0x0E,0x09,0x1C,0x1B,0x12,0x15,0x38,0x3F,0x36,0x31,0x24,0x23,0x2A,0x2D,
    0x70,0x77,0x7E,0x79,0x6C,0x6B,0x62,0x65,0x48,0x4F,0x46,0x41,0x54,0x53,0x5A,0x5D,
    0xE0,0xE7,0xEE,0xE9,0xFC,0xFB,0xF2,0xF5,0xD8,0xDF,0xD6,0xD1,0xC4,0xC3,0xCA,0xCD,
    0x90,0x97,0x9E,0x99,0x8C,0x8B,0x82,0x85,0xA8,0xAF,0xA6,0xA1,0xB4,0xB3,0xBA,0xBD,
    0xC7,0xC0,0xC9,0xCE,0xDB,0xDC,0xD5,0xD2,0xFF,0xF8,0xF1,0xF6,0xE3,0xE4,0xED,0xEA,
    0xB7,0xB0,0xB9,0xBE,0xAB,0xAC,0xA5,0xA2,0x8F,0x88,0x81,0x86,0x93,0x94,0x9D,0x9A,
    0x27,0x20,0x29,0x2E,0x3B,0x3C,0x35,0x32,0x1F,0x18,0x11,0x16,0x03,0x04,0x0D,0x0A,
    0x57,0x50,0x59,0x5E,0x4B,0x4C,0x45,0x42,0x6F,0x68,0x61,0x66,0x73,0x74,0x7D,0x7A,
    0x89,0x8E,0x87,0x80,0x95,0x92,0x9B,0x9C,0xB1,0xB6,0xBF,0xB8,0xAD,0xAA,0xA3,0xA4,
    0xF9,0xFE,0xF7,0xF0,0xE5,0xE2,0xEB,0xEC,0xC1,0xC6,0xCF,0xC8,0xDD,0xDA,0xD3,0xD4,
    0x69,0x6E,0x67,0x60,0x75,0x72,0x7B,0x7C,0x51,0x56,0x5F,0x58,0x4D,0x4A,0x43,0x44,
    0x19,0x1E,0x17,0x10,0x05,0x02,0x0B,0x0C,0x21,0x26,0x2F,0x28,0x3D,0x3A,0x33,0x34,
    0x4E,0x49,0x40,0x47,0x52,0x55,0x5C,0x5B,0x76,0x71,0x78,0x7F,0x6A,0x6D,0x64,0x63,
    0x3E,0x39,0x30,0x37,0x22,0x25,0x2C,0x2B,0x06,0x01,0x08,0x0F,0x1A,0x1D,0x14,0x13,
    0xAE,0xA9,0xA0,0xA7,0xB2,0xB5,0xBC,0xBB,0x96,0x91,0x98,0x9F,0x8A,0x8D,0x84,0x83,
    0xDE,0xD9,0xD0,0xD7,0xC2,0xC5,0xCC,0xCB,0xE6,0xE1,0xE8,0xEF,0xFA,0xFD,0xF4,0xF3
};

FSIAnalyzer::FSIAnalyzer()
    : Analyzer2(),
      mClock(nullptr), mData0(nullptr), mData1(nullptr),
      mSampleRateHz(0), mTwoLane(false), mNWordCount(16), mSpiCompatMode(false),
      mLastClockSample(0), mLastClockState(BIT_LOW)
{
    SetAnalyzerSettings( &mSettings );
    UseFrameV2();
}

FSIAnalyzer::~FSIAnalyzer()
{
    KillThread();
}

void FSIAnalyzer::SetupResults()
{
    mResults.reset( new FSIAnalyzerResults( this, &mSettings ) );
    SetAnalyzerResults( mResults.get() );
    mResults->AddChannelBubblesWillAppearOn( mSettings.mClockChannel );
    mResults->AddChannelBubblesWillAppearOn( mSettings.mDataChannel0 );
    if( mSettings.mTwoLane )
        mResults->AddChannelBubblesWillAppearOn( mSettings.mDataChannel1 );
}

U32 FSIAnalyzer::DataWordCount( U8 frame_type ) const
{
    switch( frame_type )
    {
    case 0x2: return 1;
    case 0x3: return 2;
    case 0x4: return 4;
    case 0x5: return 6;
    case 0x6: return mNWordCount;   // from UI setting
    default:  return 0;             // PING and ERROR have no data words
    }
}

void FSIAnalyzer::AdvanceToNextClockEdge( BitState& lane0_bit, BitState& lane1_bit )
{
    mClock->AdvanceToNextEdge();
    U64 clk_sample = mClock->GetSampleNumber();

    mData0->AdvanceToAbsPosition( clk_sample );
    lane0_bit = mData0->GetBitState();

    lane1_bit = BIT_LOW;
    if( mData1 )
    {
        mData1->AdvanceToAbsPosition( clk_sample );
        lane1_bit = mData1->GetBitState();
    }

    mLastClockSample = clk_sample;
}

// 2-lane interleaving fix:
// Each clock edge delivers two bits: D0=even position, D1=odd position.
// ceil(count/2) edges are needed to collect 'count' bits.
bool FSIAnalyzer::CollectBits( U32 count, U64& value,
                                U64& start_sample, U64& end_sample )
{
    value        = 0;
    start_sample = 0;
    end_sample   = 0;

    if( mTwoLane )
    {
        U32 edges_needed = ( count + 1 ) / 2;

        for( U32 edge = 0; edge < edges_needed; edge++ )
        {
            BitState b0, b1;
            AdvanceToNextClockEdge( b0, b1 );
            U64 s = mClock->GetSampleNumber();
            if( edge == 0 )               start_sample = s;
            if( edge == edges_needed - 1 ) end_sample  = s;

            // Even-indexed logical bit
            U32 even_bit_pos = edge * 2;
            if( even_bit_pos < count )
                value = ( value << 1 ) | ( b0 == BIT_HIGH ? 1u : 0u );

            // Odd-indexed logical bit (only if within count)
            U32 odd_bit_pos = edge * 2 + 1;
            if( odd_bit_pos < count )
                value = ( value << 1 ) | ( b1 == BIT_HIGH ? 1u : 0u );
        }
    }
    else
    {
        for( U32 i = 0; i < count; i++ )
        {
            BitState b0, b1;
            AdvanceToNextClockEdge( b0, b1 );
            U64 s = mClock->GetSampleNumber();
            if( i == 0 )         start_sample = s;
            if( i == count - 1 ) end_sample   = s;

            value = ( value << 1 ) | ( b0 == BIT_HIGH ? 1u : 0u );
        }
    }

    return true;
}

// CRC over std::vector — no fixed buffer, no overflow possible
U8 FSIAnalyzer::ComputeCRC( const std::vector<U8>& data )
{
    U8 crc = 0x00;
    for( U8 byte : data )
        crc = kFsiCrcTable[ crc ^ byte ];
    return crc;
}

bool FSIAnalyzer::SyncPreamble( U64& frame_start_sample )
{
    enum State { HUNT_FLUSH, HUNT_IDLE, HUNT_SOF };
    State state = HUNT_FLUSH;

    U32 alternating_count = 0;
    U32 idle_count        = 0;
    U32 sof_bit_index     = 0;
    U8  sof_expected[]    = { 1, 0, 1, 0 };   // SOF nibble = 0xA, MSB first
    U64 preamble_start    = 0;
    BitState prev_b0      = BIT_LOW;
    bool first            = true;

    while( true )
    {
        BitState b0, b1;
        AdvanceToNextClockEdge( b0, b1 );
        U64 s = mClock->GetSampleNumber();

        if( ( s & 0xFFFFF ) == 0 )
            ReportProgress( s );

        switch( state )
        {
        case HUNT_FLUSH:
            if( first )
            {
                preamble_start    = s;
                prev_b0           = b0;
                alternating_count = 1;
                first             = false;
            }
            else if( b0 != prev_b0 )
            {
                alternating_count++;
                prev_b0 = b0;
            }
            else
            {
                if( alternating_count >= 6 && b0 == BIT_LOW )
                {
                    idle_count = 1;
                    state      = HUNT_IDLE;
                }
                else
                {
                    alternating_count = 1;
                    prev_b0           = b0;
                    preamble_start    = s;
                }
            }
            break;

        case HUNT_IDLE:
            if( b0 == BIT_LOW )
            {
                idle_count++;
                if( idle_count >= 2 )
                {
                    sof_bit_index = 0;
                    state         = HUNT_SOF;
                }
            }
            else
            {
                if( sof_expected[0] == 1 )
                {
                    sof_bit_index = 1;
                    state         = HUNT_SOF;
                }
                else
                {
                    state             = HUNT_FLUSH;
                    alternating_count = 1;
                    prev_b0           = b0;
                }
            }
            break;

        case HUNT_SOF:
        {
            U8 got = ( b0 == BIT_HIGH ) ? 1 : 0;
            if( got == sof_expected[sof_bit_index] )
            {
                sof_bit_index++;
                if( sof_bit_index == 4 )
                {
                    Frame pf;
                    pf.mStartingSampleInclusive = preamble_start;
                    pf.mEndingSampleInclusive   = s;
                    pf.mType  = FSI_RESULT_PREAMBLE;
                    pf.mData1 = 0;
                    pf.mData2 = 0;
                    pf.mFlags = 0;
                    mResults->AddFrame( pf );

                    FrameV2 fv2;
                    mResults->AddFrameV2( fv2, "preamble", preamble_start, s );

                    frame_start_sample = s;
                    return true;
                }
            }
            else
            {
                state             = HUNT_FLUSH;
                alternating_count = 1;
                prev_b0           = b0;
                preamble_start    = s;
            }
            break;
        }
        }
    }
}

bool FSIAnalyzer::SyncSpiCompat( U64& frame_start_sample )
{
    while( true )
    {
        mData0->AdvanceToNextEdge();
        U64 s = mData0->GetSampleNumber();

        if( ( s & 0xFFFFF ) == 0 )
            ReportProgress( s );

        if( mData0->GetBitState() == BIT_LOW )
        {
            Frame pf;
            pf.mStartingSampleInclusive = s;
            pf.mEndingSampleInclusive   = s;
            pf.mType  = FSI_RESULT_PREAMBLE;
            pf.mData1 = 1;
            pf.mData2 = 0;
            pf.mFlags = 0x02;   // bit1 = SPI-compat flag
            mResults->AddFrame( pf );

            FrameV2 fv2;
            fv2.AddString( "mode", "SPI-compat CS" );
            mResults->AddFrameV2( fv2, "preamble", s, s );

            if( mClock->GetBitState() == BIT_HIGH )
                mClock->AdvanceToNextEdge();
            mClock->AdvanceToNextEdge();

            frame_start_sample = mClock->GetSampleNumber();
            return true;
        }
    }
}

void FSIAnalyzer::WorkerThread()
{
    mSampleRateHz  = GetSampleRate();
    mTwoLane       = mSettings.mTwoLane;
    mNWordCount    = mSettings.mNWordCount;
    mSpiCompatMode = mSettings.mSpiCompatMode;

    mClock = GetAnalyzerChannelData( mSettings.mClockChannel );
    mData0 = GetAnalyzerChannelData( mSettings.mDataChannel0 );
    mData1 = mTwoLane ? GetAnalyzerChannelData( mSettings.mDataChannel1 ) : nullptr;

    if( mClock->GetBitState() == BIT_HIGH )
        mClock->AdvanceToNextEdge();

    while( true )
    {
        U64 frame_start;
        if( mSpiCompatMode )
        {
            if( !SyncSpiCompat( frame_start ) ) break;
        }
        else
        {
            if( !SyncPreamble( frame_start ) ) break;
        }

        mResults->CommitPacketAndStartNewPacket();

        // Header: FrameType[3:0], Tag[3:0], UserData[7:0]  (16 bits, MSB first)
        U64 hdr_val, hdr_start, hdr_end;
        CollectBits( 16, hdr_val, hdr_start, hdr_end );

        U8 frame_type = ( hdr_val >> 12 ) & 0x0F;
        U8 tag        = ( hdr_val >>  8 ) & 0x0F;
        U8 user_data  = ( hdr_val       ) & 0xFF;

        // CRC buffer — vector, no fixed-size limit
        std::vector<U8> crc_buf;
        crc_buf.reserve( 2 + mNWordCount * 2 );
        crc_buf.push_back( (U8)( hdr_val >> 8 ) );
        crc_buf.push_back( (U8)( hdr_val      ) );

        U64 hdr_span = hdr_end - hdr_start;
        U64 quarter  = hdr_span / 4;

        {
            Frame f;
            f.mStartingSampleInclusive = hdr_start;
            f.mEndingSampleInclusive   = hdr_start + quarter;
            f.mType  = FSI_RESULT_FRAME_TYPE;
            f.mData1 = frame_type; f.mData2 = 0; f.mFlags = 0;
            mResults->AddFrame( f );
            FrameV2 fv2;
            fv2.AddString( "type", FrameTypeName( frame_type ) );
            fv2.AddInteger( "value", frame_type );
            mResults->AddFrameV2( fv2, "frame_type", f.mStartingSampleInclusive, f.mEndingSampleInclusive );
        }
        {
            Frame f;
            f.mStartingSampleInclusive = hdr_start + quarter;
            f.mEndingSampleInclusive   = hdr_start + 2 * quarter;
            f.mType  = FSI_RESULT_TAG;
            f.mData1 = tag; f.mData2 = 0; f.mFlags = 0;
            mResults->AddFrame( f );
            FrameV2 fv2;
            fv2.AddInteger( "tag", tag );
            mResults->AddFrameV2( fv2, "tag", f.mStartingSampleInclusive, f.mEndingSampleInclusive );
        }
        {
            Frame f;
            f.mStartingSampleInclusive = hdr_start + 2 * quarter;
            f.mEndingSampleInclusive   = hdr_end;
            f.mType  = FSI_RESULT_USERDATA;
            f.mData1 = user_data; f.mData2 = 0; f.mFlags = 0;
            mResults->AddFrame( f );
            FrameV2 fv2;
            fv2.AddInteger( "user_data", user_data );
            mResults->AddFrameV2( fv2, "user_data", f.mStartingSampleInclusive, f.mEndingSampleInclusive );
        }

        // Data words
        U32 num_words = DataWordCount( frame_type );
        for( U32 w = 0; w < num_words; w++ )
        {
            U64 word_val, ws, we;
            CollectBits( 16, word_val, ws, we );
            crc_buf.push_back( (U8)( word_val >> 8 ) );
            crc_buf.push_back( (U8)( word_val      ) );

            Frame f;
            f.mStartingSampleInclusive = ws; f.mEndingSampleInclusive = we;
            f.mType  = FSI_RESULT_DATA_WORD;
            f.mData1 = word_val; f.mData2 = w; f.mFlags = 0;
            mResults->AddFrame( f );
            FrameV2 fv2;
            fv2.AddInteger( "word_index", w );
            fv2.AddInteger( "value", word_val );
            mResults->AddFrameV2( fv2, "data", ws, we );
        }

        // CRC
        {
            U64 crc_val, cs, ce;
            CollectBits( 8, crc_val, cs, ce );
            U8 computed_crc = ComputeCRC( crc_buf );
            bool crc_ok = ( (U8)crc_val == computed_crc );

            Frame f;
            f.mStartingSampleInclusive = cs; f.mEndingSampleInclusive = ce;
            f.mType  = FSI_RESULT_CRC;
            f.mData1 = (U8)crc_val; f.mData2 = computed_crc;
            f.mFlags = crc_ok ? 0x01 : 0x00;
            mResults->AddFrame( f );
            FrameV2 fv2;
            fv2.AddInteger( "received", (U8)crc_val );
            fv2.AddInteger( "expected", computed_crc );
            fv2.AddString(  "status", crc_ok ? "OK" : "FAIL" );
            mResults->AddFrameV2( fv2, "crc", cs, ce );
        }

        // EOF (4 bits = 0x9)
        {
            U64 eof_val, es, ee;
            CollectBits( 4, eof_val, es, ee );
            bool eof_ok = ( eof_val == 0x9 );

            Frame f;
            f.mStartingSampleInclusive = es; f.mEndingSampleInclusive = ee;
            f.mType  = eof_ok ? FSI_RESULT_EOF : FSI_RESULT_ERROR;
            f.mData1 = eof_val; f.mData2 = 0; f.mFlags = 0;
            mResults->AddFrame( f );
            FrameV2 fv2;
            fv2.AddInteger( "pattern", eof_val );
            mResults->AddFrameV2( fv2, eof_ok ? "eof" : "error", es, ee );
        }

        mResults->CommitResults();
        ReportProgress( mClock->GetSampleNumber() );
    }
}

U32  FSIAnalyzer::GetMinimumSampleRateHz() { return 4000000; }
const char* FSIAnalyzer::GetAnalyzerName() const { return "TI FSI (C2000)"; }
bool FSIAnalyzer::NeedsRerun() { return false; }
U32  FSIAnalyzer::GenerateSimulationData( U64, U32, SimulationChannelDescriptor** ) { return 0; }

const char* FrameTypeName( U64 ft )
{
    switch( ft )
    {
    case 0x0: return "PING";
    case 0x1: return "ERROR";
    case 0x2: return "DATA(1w)";
    case 0x3: return "DATA(2w)";
    case 0x4: return "DATA(4w)";
    case 0x5: return "DATA(6w)";
    case 0x6: return "DATA(Nw)";
    default:  return "UNKNOWN";
    }
}

const char* GetAnalyzerName()               { return "TI FSI (C2000)"; }
Analyzer*   CreateAnalyzer()                { return new FSIAnalyzer(); }
void        DestroyAnalyzer( Analyzer* a )  { delete a; }
```

-----

## Known remaining limitations

- **N-word frame actual count** is set via the UI dropdown — you must match it to `FSI_TX_FRAME_CTRL.N_WORDS` in your firmware. It cannot be inferred from the wire.
- **CRC polynomial** may need adjustment per device revision. If CRC shows BAD on known-good captures, regenerate `kFsiCrcTable[]` using the exact polynomial in your TRM's FSI chapter.
- **`SyncSpiCompat()`** watches for TXD0 falling edges. If your board has pull-up noise during CS deassertion, add a sample count threshold before committing to a frame start.
- **`GenerateSimulationData()`** returns 0 — no simulated capture is provided. Test against real hardware.
- **macOS and Windows lib paths** in `CMakeLists.txt` still reference the legacy `lib/` directory. The bundled SDK ships `lib_arm64/` on macOS — update `SALEAE_LIB` accordingly if building on Apple Silicon.
