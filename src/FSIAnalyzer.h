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
    // In 1-lane: lane0_bit is the bit on TXDA.
    // In 2-lane: lane0_bit = TXDA (even bit), lane1_bit = TXDB (odd bit).
    void AdvanceToNextClockEdge( BitState& lane0_bit, BitState& lane1_bit );

    // Collect 'count' logical bits into a value (MSB first).
    // 1-lane: reads 'count' DDR edges from TXDA only.
    // 2-lane: reads ceil(count/2) edges; each edge delivers D0 (even) + D1 (odd).
    bool CollectBits( U32 count, U64& value,
                      U64& start_sample, U64& end_sample );

    // Returns the number of data words for a given frame type.
    // Uses mNWordCount for type 0x6.
    U32  DataWordCount( U8 frame_type ) const;

    // Detect flush+SOF preamble (normal FSI mode).
    bool SyncPreamble( U64& frame_start_sample );

    // Detect SPI-compatible frame start (CS assertion = TXDA going LOW).
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
