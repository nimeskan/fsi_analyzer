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
const char* FSIAnalyzer::GetAnalyzerName() const { return "TI FSI"; }
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

const char* GetAnalyzerName()               { return "TI FSI"; }
Analyzer*   CreateAnalyzer()                { return new FSIAnalyzer(); }
void        DestroyAnalyzer( Analyzer* a )  { delete a; }
