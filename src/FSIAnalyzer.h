// TI FSI Analyzer
// Author: Nima Eskandari
// A Saleae Logic 2 Low-Level Analyzer plugin for Texas Instruments Fast Serial Interface

#pragma once
#include <Analyzer.h>
#include <vector>
#include "FSIAnalyzerSettings.h"
#include "FSIAnalyzerResults.h"

// -----------------------------------------------------------------------
// FSI Frame Type 4-bit codes (TRM Table 31-5)
// -----------------------------------------------------------------------
#define FSI_FRAME_TYPE_PING   0x0   // 0000 — heartbeat
#define FSI_FRAME_TYPE_ERROR  0xF   // 1111 — error / attention
#define FSI_FRAME_TYPE_DATA1  0x4   // 0100 — 1 data word
#define FSI_FRAME_TYPE_DATA2  0x5   // 0101 — 2 data words
#define FSI_FRAME_TYPE_DATA4  0x6   // 0110 — 4 data words
#define FSI_FRAME_TYPE_DATA6  0x7   // 0111 — 6 data words
#define FSI_FRAME_TYPE_NWORD  0x3   // 0011 — N data words (software configured)

// -----------------------------------------------------------------------
// Saleae result frame types emitted by this analyzer
// -----------------------------------------------------------------------
#define FSI_RESULT_PREAMBLE     0x00
#define FSI_RESULT_FRAME_TYPE   0x01
#define FSI_RESULT_TAG          0x02
#define FSI_RESULT_USERDATA     0x03
#define FSI_RESULT_DATA_WORD    0x04
#define FSI_RESULT_CRC          0x05
#define FSI_RESULT_EOF          0x06
#define FSI_RESULT_SOF          0x07
#define FSI_RESULT_POSTAMBLE    0x08
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
    // Advance to next DDR clock edge and sample both data lanes.
    void AdvanceToNextClockEdge( BitState& lane0_bit, BitState& lane1_bit );

    // Collect 'count' logical bits, MSB first.
    //
    // interleaved = false  (control fields: Frame Type, Frame Tag, EOF, postamble)
    //   Always reads 'count' edges from D0 only.  In 2-lane mode these fields
    //   are transmitted complete and identical on both lanes, so D0 suffices.
    //
    // interleaved = true   (data fields: User Data, Data Words, CRC)
    //   1-lane: reads 'count' edges from D0.
    //   2-lane: reads ceil(count/2) edges; each edge delivers D0 (even-position
    //           bit) and D1 (odd-position bit) simultaneously.
    bool CollectBits( U32 count, U64& value,
                      U64& start_sample, U64& end_sample,
                      bool interleaved = true );

    // Returns the number of 16-bit data words for a given frame type.
    U32  DataWordCount( U8 frame_type ) const;

    // Scan for preamble (4 clocks HIGH) + SOF (1001). Returns true when found.
    bool SyncPreamble( U64& frame_start_sample );

    // CRC-8 over a byte vector (poly 0x07, seed 0x00, no final XOR).
    U8   ComputeCRC( const std::vector<U8>& data );

    FSIAnalyzerSettings mSettings;
    std::unique_ptr<FSIAnalyzerResults> mResults;
    AnalyzerChannelData* mClock;
    AnalyzerChannelData* mData0;
    AnalyzerChannelData* mData1;    // nullptr in 1-lane mode

    U32  mSampleRateHz;
    bool mTwoLane;
    U32  mNWordCount;

    U64 mLastClockSample;
};

extern "C" ANALYZER_EXPORT const char* __cdecl GetAnalyzerName();
extern "C" ANALYZER_EXPORT Analyzer*   __cdecl CreateAnalyzer();
extern "C" ANALYZER_EXPORT void        __cdecl DestroyAnalyzer( Analyzer* analyzer );
