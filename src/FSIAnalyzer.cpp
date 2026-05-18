#include "FSIAnalyzer.h"
#include "FSIAnalyzerResults.h"
#include "FSIAnalyzerSettings.h"
#include <AnalyzerHelpers.h>
#include <AnalyzerChannelData.h>
#include <vector>

const char* FrameTypeName( U64 ft );
static bool IsDataFrame( U8 ft );

// ============================================================================
//  FSI CRC-8
//  Polynomial: x^8 + x^2 + x + 1 (0x07), seed 0x00, no final XOR.
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

// ============================================================================

FSIAnalyzer::FSIAnalyzer()
    : Analyzer2(),
      mClock(nullptr), mData0(nullptr), mData1(nullptr),
      mSampleRateHz(0), mTwoLane(false), mNWordCount(16),
      mLastClockSample(0)
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

// Returns the number of 16-bit data words for a given frame type.
U32 FSIAnalyzer::DataWordCount( U8 frame_type ) const
{
    switch( frame_type )
    {
    case FSI_FRAME_TYPE_DATA1: return 1;
    case FSI_FRAME_TYPE_DATA2: return 2;
    case FSI_FRAME_TYPE_DATA4: return 4;
    case FSI_FRAME_TYPE_DATA6: return 6;
    case FSI_FRAME_TYPE_NWORD: return mNWordCount;
    default:                   return 0;
    }
}

// Returns true for frame types that carry User Data, Data Words, and CRC.
static bool IsDataFrame( U8 ft )
{
    return ft == FSI_FRAME_TYPE_DATA1 || ft == FSI_FRAME_TYPE_DATA2 ||
           ft == FSI_FRAME_TYPE_DATA4 || ft == FSI_FRAME_TYPE_DATA6 ||
           ft == FSI_FRAME_TYPE_NWORD;
}

// ============================================================================
//  Signal sampling
// ============================================================================

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

// ============================================================================
//  Bit collection
//
//  interleaved = false  — control fields (Frame Type, Frame Tag, EOF, postamble)
//    TRM: these fields are transmitted complete and identical on both lanes in
//    2-lane mode.  Always read N edges from D0 only.
//
//  interleaved = true   — data fields (User Data, Data Words, CRC)
//    1-lane: N edges, D0 only.
//    2-lane: ceil(N/2) edges; each edge: D0 = even-position bit, D1 = odd.
// ============================================================================

bool FSIAnalyzer::CollectBits( U32 count, U64& value,
                                U64& start_sample, U64& end_sample,
                                bool interleaved )
{
    value        = 0;
    start_sample = 0;
    end_sample   = 0;

    if( mTwoLane && interleaved )
    {
        U32 edges = ( count + 1 ) / 2;
        for( U32 e = 0; e < edges; e++ )
        {
            BitState b0, b1;
            AdvanceToNextClockEdge( b0, b1 );
            U64 s = mClock->GetSampleNumber();
            if( e == 0 )          start_sample = s;
            if( e == edges - 1 )  end_sample   = s;

            U32 pos_even = e * 2;
            if( pos_even < count )
                value = ( value << 1 ) | ( b0 == BIT_HIGH ? 1u : 0u );

            U32 pos_odd = e * 2 + 1;
            if( pos_odd < count )
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
            if( i == 0 )           start_sample = s;
            if( i == count - 1 )   end_sample   = s;

            value = ( value << 1 ) | ( b0 == BIT_HIGH ? 1u : 0u );
        }
    }

    return true;
}

// ============================================================================
//  CRC
// ============================================================================

U8 FSIAnalyzer::ComputeCRC( const std::vector<U8>& data )
{
    U8 crc = 0x00;
    for( U8 byte : data )
        crc = kFsiCrcTable[ crc ^ byte ];
    return crc;
}

// ============================================================================
//  Frame synchronisation
//
//  FSI preamble (TRM §31.3.4.1):
//    Idle    : data lines HIGH
//    Preamble: 4 clock edges, data HIGH  (= 1111)
//    SOF     : 4 bits = 1001
//
//  Since idle is also HIGH, preamble and idle are indistinguishable.
//  We detect frame start by scanning for >= 5 consecutive HIGH bits
//  (4 preamble + SOF[0]=1) followed by SOF[1..3] = 0, 0, 1.
//
//  A ring buffer of the 5 most-recent HIGH sample positions lets us
//  place the preamble bubble start exactly 4 edges before SOF[0].
// ============================================================================

bool FSIAnalyzer::SyncPreamble( U64& frame_start_sample )
{
    U32 high_count   = 0;
    U64 s_ring[5]    = { 0, 0, 0, 0, 0 };
    U32 r            = 0;   // ring write index

    while( true )
    {
        BitState b0, b1;
        AdvanceToNextClockEdge( b0, b1 );
        U64 s = mClock->GetSampleNumber();

        if( ( s & 0xFFFFF ) == 0 )
            ReportProgress( s );

        if( b0 == BIT_HIGH )
        {
            s_ring[ r % 5 ] = s;
            r++;
            high_count++;
        }
        else  // BIT_LOW — potential SOF[1] = 0
        {
            if( high_count >= 5 )
            {
                // Read SOF[2] — must be LOW
                BitState b2, dummy;
                U64 sof1_sample = mClock->GetSampleNumber();  // Start of SOF[1]
                AdvanceToNextClockEdge( b2, dummy );

                if( b2 == BIT_LOW )
                {
                    // Read SOF[3] — must be HIGH
                    BitState b3;
                    AdvanceToNextClockEdge( b3, dummy );
                    U64 s3 = mClock->GetSampleNumber();

                    if( b3 == BIT_HIGH )
                    {
                        // SOF = 1001 confirmed.
                        // Preamble = the 4 HIGH edges before SOF[0].
                        // SOF[0] is the last HIGH stored: s_ring[(r-1)%5].
                        // Preamble starts 4 edges before that:  s_ring[(r-5)%5] = s_ring[r%5].
                        // Preamble bubble ends at SOF[0] (last HIGH).
                        U64 pre_start = ( r >= 5 ) ? s_ring[ r % 5 ] : s_ring[ 0 ];
                        U64 pre_end   = s_ring[ ( r - 1 + 5 ) % 5 ];   // SOF[0] sample

                        Frame pf;
                        pf.mStartingSampleInclusive = pre_start;
                        pf.mEndingSampleInclusive   = pre_end;
                        pf.mType  = FSI_RESULT_PREAMBLE;
                        pf.mData1 = 0; pf.mData2 = 0; pf.mFlags = 0;
                        mResults->AddFrame( pf );

                        FrameV2 fv2;
                        mResults->AddFrameV2( fv2, "preamble", pre_start, pre_end );

                        // Emit SOF frame (SOF[1,2,3])
                        Frame sf;
                        sf.mStartingSampleInclusive = sof1_sample;
                        sf.mEndingSampleInclusive   = s3;
                        sf.mType  = FSI_RESULT_SOF;
                        sf.mData1 = 0; sf.mData2 = 0; sf.mFlags = 0;
                        mResults->AddFrame( sf );

                        FrameV2 sfv2;
                        mResults->AddFrameV2( sfv2, "sof", sof1_sample, s3 );

                        frame_start_sample = s3;
                        return true;
                    }
                }
            }
            // Not a valid SOF — reset and keep scanning
            high_count = 0;
            r          = 0;
        }
    }
}

// ============================================================================
//  Main decode loop
//
//  Frame structure per TRM Table 31-4 / §31.3.4.1:
//
//    ALL frames:
//      Preamble (4 clocks HIGH) + SOF (1001)  ← detected by SyncPreamble()
//      Frame Type  : 4 bits  control field
//      [data frames only:]
//        User Data : 8 bits  interleaved
//        Data Words: N×16b   interleaved
//        CRC       : 8 bits  interleaved
//      Frame Tag   : 4 bits  control field
//      EOF         : 4 bits  control field  = 0110
//      Postamble   : 4 clocks HIGH          = 1111  (consumed silently)
// ============================================================================

void FSIAnalyzer::WorkerThread()
{
    mSampleRateHz = GetSampleRate();
    mTwoLane      = mSettings.mTwoLane;
    mNWordCount   = mSettings.mNWordCount;

    mClock = GetAnalyzerChannelData( mSettings.mClockChannel );
    mData0 = GetAnalyzerChannelData( mSettings.mDataChannel0 );
    mData1 = mTwoLane ? GetAnalyzerChannelData( mSettings.mDataChannel1 ) : nullptr;

    // FSI idle state is CLK=HIGH with no clock edges.  The first edge in any
    // capture is always the falling edge that opens the preamble.  Do NOT
    // pre-advance: if CLK is HIGH here we are in idle, and AdvanceToNextEdge
    // would consume preamble bit 1 before SyncPreamble ever reads it.

    while( true )
    {
        U64 frame_start;
        if( !SyncPreamble( frame_start ) ) break;

        mResults->CommitPacketAndStartNewPacket();

        // ---- Frame Type (4 bits, control field) ----
        U64 ft_val, ft_start, ft_end;
        CollectBits( 4, ft_val, ft_start, ft_end, false );
        U8 frame_type = (U8)ft_val;

        {
            Frame f;
            f.mStartingSampleInclusive = ft_start;
            f.mEndingSampleInclusive   = ft_end;
            f.mType  = FSI_RESULT_FRAME_TYPE;
            f.mData1 = frame_type; f.mData2 = 0; f.mFlags = 0;
            mResults->AddFrame( f );
            FrameV2 fv2;
            fv2.AddString( "type", FrameTypeName( frame_type ) );
            fv2.AddInteger( "value", frame_type );
            mResults->AddFrameV2( fv2, "frame_type", ft_start, ft_end );
        }

        // ---- User Data, Data Words, CRC (data frames only) ----
        if( IsDataFrame( frame_type ) )
        {
            std::vector<U8> crc_buf;
            crc_buf.reserve( 1 + mNWordCount * 2 );

            // User Data — 8 bits, interleaved
            U64 ud_val, ud_start, ud_end;
            CollectBits( 8, ud_val, ud_start, ud_end, true );
            U8 user_data = (U8)ud_val;
            crc_buf.push_back( user_data );

            {
                Frame f;
                f.mStartingSampleInclusive = ud_start;
                f.mEndingSampleInclusive   = ud_end;
                f.mType  = FSI_RESULT_USERDATA;
                f.mData1 = user_data; f.mData2 = 0; f.mFlags = 0;
                mResults->AddFrame( f );
                FrameV2 fv2;
                fv2.AddInteger( "user_data", user_data );
                mResults->AddFrameV2( fv2, "user_data", ud_start, ud_end );
            }

            // Data Words — interleaved, word 0 first
            U32 num_words = DataWordCount( frame_type );
            for( U32 w = 0; w < num_words; w++ )
            {
                U64 word_val, ws, we;
                CollectBits( 16, word_val, ws, we, true );
                crc_buf.push_back( (U8)( word_val      ) );   // LSB first
                crc_buf.push_back( (U8)( word_val >> 8 ) );

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

            // CRC — 8 bits, interleaved
            U64 crc_val, cs, ce;
            CollectBits( 8, crc_val, cs, ce, true );
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

        // ---- Frame Tag (4 bits, control field, present in all frame types) ----
        U64 tag_val, tag_start, tag_end;
        CollectBits( 4, tag_val, tag_start, tag_end, false );

        {
            Frame f;
            f.mStartingSampleInclusive = tag_start;
            f.mEndingSampleInclusive   = tag_end;
            f.mType  = FSI_RESULT_TAG;
            f.mData1 = tag_val; f.mData2 = 0; f.mFlags = 0;
            mResults->AddFrame( f );
            FrameV2 fv2;
            fv2.AddInteger( "tag", tag_val );
            mResults->AddFrameV2( fv2, "tag", tag_start, tag_end );
        }

        // ---- EOF (4 bits = 0110, control field) ----
        U64 eof_val, es, ee;
        CollectBits( 4, eof_val, es, ee, false );
        bool eof_ok = ( eof_val == 0x6 );   // 0110

        {
            Frame f;
            f.mStartingSampleInclusive = es; f.mEndingSampleInclusive = ee;
            f.mType  = eof_ok ? FSI_RESULT_EOF : FSI_RESULT_ERROR;
            f.mData1 = eof_val; f.mData2 = 0; f.mFlags = 0;
            mResults->AddFrame( f );
            FrameV2 fv2;
            fv2.AddInteger( "pattern", eof_val );
            mResults->AddFrameV2( fv2, eof_ok ? "eof" : "error", es, ee );
        }

        // ---- Postamble (4 clocks HIGH = 1111, control field) ----
        U64 post_val, ps, pe;
        CollectBits( 4, post_val, ps, pe, false );

        {
            Frame f;
            f.mStartingSampleInclusive = ps;
            f.mEndingSampleInclusive   = pe;
            f.mType  = FSI_RESULT_POSTAMBLE;
            f.mData1 = 0; f.mData2 = 0; f.mFlags = 0;
            mResults->AddFrame( f );

            FrameV2 fv2;
            mResults->AddFrameV2( fv2, "postamble", ps, pe );
        }

        mResults->CommitResults();
        ReportProgress( mClock->GetSampleNumber() );
    }
}

// ============================================================================
//  SDK entry points
// ============================================================================

U32  FSIAnalyzer::GetMinimumSampleRateHz() { return 4000000; }
const char* FSIAnalyzer::GetAnalyzerName() const { return "TI FSI"; }
bool FSIAnalyzer::NeedsRerun() { return false; }
U32  FSIAnalyzer::GenerateSimulationData( U64, U32, SimulationChannelDescriptor** ) { return 0; }

const char* FrameTypeName( U64 ft )
{
    switch( ft )
    {
    case FSI_FRAME_TYPE_PING:  return "PING";
    case FSI_FRAME_TYPE_ERROR: return "ERROR";
    case FSI_FRAME_TYPE_DATA1: return "DATA(1w)";
    case FSI_FRAME_TYPE_DATA2: return "DATA(2w)";
    case FSI_FRAME_TYPE_DATA4: return "DATA(4w)";
    case FSI_FRAME_TYPE_DATA6: return "DATA(6w)";
    case FSI_FRAME_TYPE_NWORD: return "DATA(Nw)";
    default:                   return "UNKNOWN";
    }
}

const char* GetAnalyzerName()               { return "TI FSI"; }
Analyzer*   CreateAnalyzer()                { return new FSIAnalyzer(); }
void        DestroyAnalyzer( Analyzer* a )  { delete a; }
