# FSI Analyzer — Packet Detection State Machine

This document walks through every step the analyzer takes from the moment
Logic 2 starts playback to the moment a fully decoded FSI frame appears on
screen. Each step is traced directly to the code that implements it.

---

## Overview: how the plugin starts

Logic 2 loads the shared library and calls three C-linkage entry points in order:

```cpp
// FSIAnalyzer.cpp — bottom of file
const char* GetAnalyzerName()               { return "TI FSI"; }
Analyzer*   CreateAnalyzer()                { return new FSIAnalyzer(); }
void        DestroyAnalyzer( Analyzer* a )  { delete a; }
```

`CreateAnalyzer()` constructs `FSIAnalyzer`. The constructor registers the
settings object and opts into the FrameV2 API:

```cpp
// FSIAnalyzer.cpp:36
FSIAnalyzer::FSIAnalyzer()
    : Analyzer2(),
      mClock(nullptr), mData0(nullptr), mData1(nullptr),
      mSampleRateHz(0), mTwoLane(false), mNWordCount(16),
      mLastClockSample(0)
{
    SetAnalyzerSettings( &mSettings );
    UseFrameV2();
}
```

Logic 2 then calls `SetupResults()`, which registers which channels will show
bubble labels:

```cpp
// FSIAnalyzer.cpp:51
void FSIAnalyzer::SetupResults()
{
    mResults.reset( new FSIAnalyzerResults( this, &mSettings ) );
    SetAnalyzerResults( mResults.get() );
    mResults->AddChannelBubblesWillAppearOn( mSettings.mClockChannel );
    mResults->AddChannelBubblesWillAppearOn( mSettings.mDataChannel0 );
    if( mSettings.mTwoLane )
        mResults->AddChannelBubblesWillAppearOn( mSettings.mDataChannel1 );
}
```

Finally, Logic 2 calls `WorkerThread()` on a background thread. Everything
from this point forward is the state machine.

---

## Step 1 — WorkerThread initialisation

`WorkerThread()` reads the user settings and obtains live channel data handles:

```cpp
// FSIAnalyzer.cpp:273
void FSIAnalyzer::WorkerThread()
{
    mSampleRateHz = GetSampleRate();
    mTwoLane      = mSettings.mTwoLane;
    mNWordCount   = mSettings.mNWordCount;

    mClock = GetAnalyzerChannelData( mSettings.mClockChannel );
    mData0 = GetAnalyzerChannelData( mSettings.mDataChannel0 );
    mData1 = mTwoLane ? GetAnalyzerChannelData( mSettings.mDataChannel1 ) : nullptr;
```

`mData1` is `nullptr` in 1-lane mode. Every part of the code that reads D1
guards on this pointer first.

No pre-advance is performed on the clock cursor. FSI idle state is CLK=HIGH
with no clock edges — the clock only runs during frame transmission. Because
of this, "CLK is HIGH at capture start" always means the capture started
during idle, and the very next clock edge will be the first falling edge of the
preamble. Pre-advancing to that falling edge would consume preamble bit 1
before `SyncPreamble` ever reads it, causing the first frame to be missed.

```cpp
// FSIAnalyzer.cpp:283
    // FSI idle state is CLK=HIGH with no clock edges.  The first edge in any
    // capture is always the falling edge that opens the preamble.  Do NOT
    // pre-advance: if CLK is HIGH here we are in idle, and AdvanceToNextEdge
    // would consume preamble bit 1 before SyncPreamble ever reads it.
```

**State after Step 1:** the clock cursor sits at position 0 of the capture.
The first `AdvanceToNextEdge()` call inside `SyncPreamble` will land on the
very first clock edge in the capture — which is preamble bit 1.

---

## Step 2 — The main loop and SyncPreamble

The outer loop runs forever. Each iteration decodes one complete FSI frame:

```cpp
// FSIAnalyzer.cpp:286
    while( true )
    {
        U64 frame_start;
        if( !SyncPreamble( frame_start ) ) break;
```

`SyncPreamble()` is where idle time is spent. It blocks until a valid
preamble + SOF is found, then returns. The `break` only fires if the SDK
signals that the capture is exhausted (which the current implementation does
not explicitly detect — the SDK will throw internally to unwind the thread).

---

## Step 3 — The sampling primitive: AdvanceToNextClockEdge

Every bit read anywhere in the analyzer goes through this single function:

```cpp
// FSIAnalyzer.cpp:87
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
```

What it does on every call:

1. **Advance the clock cursor** to the next edge (rising or falling — FSI is
   DDR so both are active).
2. **Record the exact sample number** of that edge.
3. **Snap the D0 cursor** to that same sample number with
   `AdvanceToAbsPosition()`. This is critical: the data lines and clock lines
   are captured independently; without this alignment step, the data cursor
   might be ahead or behind the clock edge.
4. **Read D0** at that sample.
5. **Snap and read D1** the same way, if in 2-lane mode.

`lane1_bit` defaults to `BIT_LOW` in 1-lane mode — the rest of the code can
read it safely without checking the mode each time.

---

## Step 4 — Preamble and SOF detection (SyncPreamble)

This is the heart of idle scanning. The function's job is to find the bit
pattern that marks the start of an FSI frame:

```
Idle     : D0 = 1 (indefinite, no clock edges required)
Preamble : 4 clock edges, D0 = 1  →  1 1 1 1
SOF      : 4 clock edges, D0 = 1 0 0 1
```

Because idle is also HIGH, preamble and idle look identical on the wire.
The only distinguishing feature is the LOW in SOF bit 1. The detection
strategy is:

> Scan for **5 or more consecutive HIGH bits** (4 preamble bits + SOF bit 0
> which is HIGH), then look ahead for SOF bits 1, 2, 3 = 0, 0, 1.

### 4a — Ring buffer and HIGH accumulation

```cpp
// FSIAnalyzer.cpp:190
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
```

`ReportProgress()` keeps the Logic 2 progress bar alive during long idle
periods. The mask `0xFFFFF` fires roughly once every million samples.

```cpp
        if( b0 == BIT_HIGH )
        {
            s_ring[ r % 5 ] = s;
            r++;
            high_count++;
        }
```

Every consecutive HIGH bit: store its sample number in a 5-slot ring buffer
and increment the counter. The ring buffer overwrites oldest entries first
(modulo 5). When `high_count` reaches 5 or more, `s_ring[r % 5]` holds the
sample 5 edges ago — i.e., the sample where the preamble began.

### 4b — Any LOW resets the accumulator

The `else` branch handles every LOW bit — regardless of how many HIGHs
preceded it:

```cpp
        else  // BIT_LOW — potential SOF[1] = 0
        {
            if( high_count >= 5 )
            {
                // ... SOF peek ...
            }
            // Not a valid SOF — reset and keep scanning
            high_count = 0;    // ← OUTSIDE the if(high_count>=5) block
            r          = 0;
        }
```

The reset `high_count = 0; r = 0;` sits **outside** the
`if( high_count >= 5 )` guard, inside the `else` block. This means it
executes for **every LOW bit** — whether the count was 0, 3, or 4. A
single LOW at any point tears down the entire accumulated count and ring
buffer and forces the scanner to start over from zero.

The `if( high_count >= 5 )` block is only an opportunity to attempt a SOF
confirmation first. If the attempt is skipped (count < 5) or fails, the
reset at the bottom always runs.

### 4c — SOF check when high_count ≥ 5

When a LOW arrives after 5 or more HIGHs, the first LOW is treated as
SOF[1] = 0. The code immediately peeks at the next two bits to confirm
the full SOF pattern `1 0 0 1`:

```cpp
            if( high_count >= 5 )
            {
                U64 sof1_sample = mClock->GetSampleNumber();   // SOF[1] sample

                // Read SOF[2] — must be LOW
                BitState b2, dummy;
                AdvanceToNextClockEdge( b2, dummy );

                if( b2 == BIT_LOW )
                {
                    // Read SOF[3] — must be HIGH
                    BitState b3;
                    AdvanceToNextClockEdge( b3, dummy );
                    U64 s3 = mClock->GetSampleNumber();

                    if( b3 == BIT_HIGH )
                    {
```

SOF = `1 0 0 1`. The four bits and their sources:
- SOF[0] = 1 — the last HIGH stored in the ring buffer
- SOF[1] = 0 — the LOW that triggered this `else` branch
- SOF[2] = 0 — read as `b2` by a new `AdvanceToNextClockEdge()` call
- SOF[3] = 1 — read as `b3` by another `AdvanceToNextClockEdge()` call

**Important:** `b2` and `b3` are consumed by calling `AdvanceToNextClockEdge()`
inside the SOF check. If the check fails partway through, those clock edges
have already been moved past and cannot be revisited:

- If `b2` is HIGH (SOF[2] mismatch): one extra edge was consumed before
  the reset. That HIGH is dropped — it does not contribute to the next
  HIGH count.
- If `b2` is LOW but `b3` is LOW (SOF[3] mismatch): two extra edges were
  consumed before the reset. Both are dropped.

After any partial SOF failure the reset fires and the loop resumes from
the edge immediately after whichever bit caused the mismatch.

### 4d — Preamble bubble emission and return

When all three SOF checks pass, the preamble bubble is placed. Two ring
buffer entries are used:

```cpp
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
```

- `pre_start = s_ring[r % 5]` — the ring slot written 5 HIGHs ago, which is
  preamble bit 1 (the first of the four preamble HIGH edges).
- `pre_end = s_ring[(r-1+5) % 5]` — the ring slot written 1 HIGH ago, which
  is preamble bit 4 (the last of the four preamble HIGH edges).

The bubble covers exactly the 4 preamble HIGHs.
SOF[0,1,2,3] were consumed during the peek. A
`FSI_RESULT_SOF` (0x07) bubble is then emitted spanning SOF[0] to SOF[3].

`frame_start_sample = s3` (the SOF[3] sample) is returned to the caller
so `WorkerThread` has a precise sample reference for the frame start, but
this sample is not part of the preamble bubble.

The bubble text rendered by `FSIAnalyzerResults` is:

```cpp
// FSIAnalyzerResults.cpp:31
    case FSI_RESULT_PREAMBLE:
        AddResultString( "PRE" );
        AddResultString( "Preamble" );
        break;
```

`return true` is the **only exit** from `SyncPreamble()` that does not
reset. Every other path through the `else` block — whether the SOF check
was never attempted or failed at b2 or b3 — falls through to the reset
before looping.

**State after Step 4:** the clock cursor is at the sample of SOF[3]. The next
`AdvanceToNextClockEdge()` call will land on the first bit of the Frame Type
field.

---

## Step 5 — The bit collection primitive: CollectBits

All remaining fields are read through `CollectBits()`:

```cpp
bool FSIAnalyzer::CollectBits( U32 count, U64& value,
                                U64& start_sample, U64& end_sample,
                                bool interleaved )
```

The `interleaved` flag controls how 2-lane mode is handled:

### Non-interleaved (interleaved = false) — control fields

Used for: Frame Type, Frame Tag, EOF, Postamble.

```cpp
// FSIAnalyzer.cpp:145
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
```

Reads exactly `count` edges. Only D0 is used. D1 is ignored even in 2-lane
mode because the TRM specifies control fields are transmitted identically on
both lanes — reading one is enough.

### Interleaved (interleaved = true, 2-lane mode) — data fields

Used for: User Data, Data Words, CRC — but only when `mTwoLane` is true.

```cpp
// FSIAnalyzer.cpp:125
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
```

Each clock edge delivers two logical bits simultaneously:
- D0 → even-indexed bit (bit position 0, 2, 4 … counting from MSB)
- D1 → odd-indexed bit (bit position 1, 3, 5 …)

Only `ceil(count/2)` edges are consumed instead of `count`. For an 8-bit
field in 2-lane mode this means 4 edges; for a 16-bit word, 8 edges.

In 1-lane mode with `interleaved = true`, the `if( mTwoLane && interleaved )`
branch is skipped and the code falls through to the simple single-lane loop
above, reading all `count` edges from D0 only.

---

## Step 6 — Frame Type (4 bits, control)

Back in `WorkerThread()`, immediately after `SyncPreamble()` returns:

```cpp
// FSIAnalyzer.cpp:294
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
```

`CollectBits(4, ..., false)` reads 4 edges from D0. The 4-bit value maps
to a frame type via `FrameTypeName()`:

```cpp
// FSIAnalyzer.cpp:424
const char* FrameTypeName( U64 ft )
{
    switch( ft )
    {
    case FSI_FRAME_TYPE_PING:  return "PING";   // 0000
    case FSI_FRAME_TYPE_ERROR: return "ERROR";  // 1111
    case FSI_FRAME_TYPE_DATA1: return "DATA(1w)"; // 0100
    case FSI_FRAME_TYPE_DATA2: return "DATA(2w)"; // 0101
    case FSI_FRAME_TYPE_DATA4: return "DATA(4w)"; // 0110
    case FSI_FRAME_TYPE_DATA6: return "DATA(6w)"; // 0111
    case FSI_FRAME_TYPE_NWORD: return "DATA(Nw)"; // 0011
    default:                   return "UNKNOWN";
    }
}
```

The bubble text displayed in Logic 2 for this field is the return value of
`FrameTypeName()` directly (e.g. `"DATA(2w)"`).

**State after Step 6:** `frame_type` is known. The code now branches on
whether this is a data-carrying frame.

---

## Step 7 — Data frame branch decision

```cpp
// FSIAnalyzer.cpp:312
        if( IsDataFrame( frame_type ) )
        {
```

```cpp
// FSIAnalyzer.cpp:76
static bool IsDataFrame( U8 ft )
{
    return ft == FSI_FRAME_TYPE_DATA1 || ft == FSI_FRAME_TYPE_DATA2 ||
           ft == FSI_FRAME_TYPE_DATA4 || ft == FSI_FRAME_TYPE_DATA6 ||
           ft == FSI_FRAME_TYPE_NWORD;
}
```

- **PING (0000) and ERROR (1111)**: `IsDataFrame` returns false. The branch
  is skipped entirely. The next step is Frame Tag.
- **DATA(1w) through DATA(Nw)**: `IsDataFrame` returns true. Steps 8–10
  execute.

---

## Step 8 — User Data (8 bits, interleaved, data frames only)

```cpp
// FSIAnalyzer.cpp:317
            std::vector<U8> crc_buf;
            crc_buf.reserve( 1 + mNWordCount * 2 );

            U64 ud_val, ud_start, ud_end;
            CollectBits( 8, ud_val, ud_start, ud_end, true );
            U8 user_data = (U8)ud_val;
            crc_buf.push_back( user_data );
```

`CollectBits(8, ..., true)` reads 8 bits: 8 edges in 1-lane mode,
4 edges in 2-lane mode (D0 and D1 simultaneously).

The User Data byte is the **first byte pushed into `crc_buf`**. This is
important — the CRC covers User Data first, before any data words.

The bubble displays `"UD"` (short) or `"UserData: 0xNN"` (long).

---

## Step 9 — Data Words (N × 16 bits, interleaved, data frames only)

`DataWordCount()` translates the frame type to a word count:

```cpp
// FSIAnalyzer.cpp:62
U32 FSIAnalyzer::DataWordCount( U8 frame_type ) const
{
    switch( frame_type )
    {
    case FSI_FRAME_TYPE_DATA1: return 1;
    case FSI_FRAME_TYPE_DATA2: return 2;
    case FSI_FRAME_TYPE_DATA4: return 4;
    case FSI_FRAME_TYPE_DATA6: return 6;
    case FSI_FRAME_TYPE_NWORD: return mNWordCount;  // from settings
    default:                   return 0;
    }
}
```

Each word is read and appended to `crc_buf` **LSB first**:

```cpp
// FSIAnalyzer.cpp:336
            U32 num_words = DataWordCount( frame_type );
            for( U32 w = 0; w < num_words; w++ )
            {
                U64 word_val, ws, we;
                CollectBits( 16, word_val, ws, we, true );
                crc_buf.push_back( (U8)( word_val      ) );   // LSB first
                crc_buf.push_back( (U8)( word_val >> 8 ) );

                Frame f;
                f.mType  = FSI_RESULT_DATA_WORD;
                f.mData1 = word_val;
                f.mData2 = w;          // zero-based word index
                f.mFlags = 0;
                // ... AddFrame / AddFrameV2 ...
            }
```

`f.mData2 = w` stores the word index so bubble text can show `Data[0]`,
`Data[1]`, etc.

The word is stored LSB-first in `crc_buf` because that is what the FSI TRM
specifies for the CRC input byte order.

In 2-lane mode, `CollectBits(16, ..., true)` consumes 8 clock edges instead
of 16.

---

## Step 10 — CRC (8 bits, interleaved, data frames only)

```cpp
// FSIAnalyzer.cpp:355
            U64 crc_val, cs, ce;
            CollectBits( 8, crc_val, cs, ce, true );
            U8 computed_crc = ComputeCRC( crc_buf );
            bool crc_ok = ( (U8)crc_val == computed_crc );

            Frame f;
            f.mType  = FSI_RESULT_CRC;
            f.mData1 = (U8)crc_val;      // received CRC byte
            f.mData2 = computed_crc;     // expected CRC byte (for debugging)
            f.mFlags = crc_ok ? 0x01 : 0x00;
```

`ComputeCRC()` runs the CRC-8 lookup table over `crc_buf`:

```cpp
// FSIAnalyzer.cpp:166
U8 FSIAnalyzer::ComputeCRC( const std::vector<U8>& data )
{
    U8 crc = 0x00;
    for( U8 byte : data )
        crc = kFsiCrcTable[ crc ^ byte ];
    return crc;
}
```

The table implements CRC-8, polynomial `x^8 + x^2 + x + 1` (0x07), seed
`0x00`, no final XOR. Input order: [User Data byte, word0_LSB, word0_MSB,
word1_LSB, word1_MSB, …].

Bubble text:

```cpp
// FSIAnalyzerResults.cpp:62
    case FSI_RESULT_CRC:
        {
            std::string s = std::string("CRC: ") + number_str;
            bool crc_ok = ( frame.mFlags & 0x01 ) != 0;
            if( !crc_ok ) s += " [BAD]";
            AddResultString( "CRC" );
            AddResultString( s.c_str() );
        }
        break;
```

**State after Step 10:** all data is decoded. `crc_buf` is discarded. The
code rejoins the path taken by PING and ERROR frames.

---

## Step 11 — Frame Tag (4 bits, control, all frames)

```cpp
// FSIAnalyzer.cpp:375
        U64 tag_val, tag_start, tag_end;
        CollectBits( 4, tag_val, tag_start, tag_end, false );

        {
            Frame f;
            f.mType  = FSI_RESULT_TAG;
            f.mData1 = tag_val;
            // ... AddFrame / AddFrameV2 ...
        }
```

Always non-interleaved (D0 only). Present in every frame type — PING, ERROR,
and all DATA frames. This is a 4-bit user-assigned sequence number the
application firmware places in each frame.

Bubble: `"TAG"` / `"Tag: N"`.

---

## Step 12 — EOF (4 bits = 0110, control, all frames)

```cpp
// FSIAnalyzer.cpp:391
        U64 eof_val, es, ee;
        CollectBits( 4, eof_val, es, ee, false );
        bool eof_ok = ( eof_val == 0x6 );   // 0110

        {
            Frame f;
            f.mType  = eof_ok ? FSI_RESULT_EOF : FSI_RESULT_ERROR;
            f.mData1 = eof_val;
            // ... AddFrame / AddFrameV2 ...
        }
```

The expected pattern is `0110` = `0x6`. If the received 4-bit value matches,
a normal EOF bubble (`"EOF"` / `"End of Frame"`) is emitted. If it does not
match, an error bubble (`"ERR"` / `"Framing Error"`) is emitted instead. In
either case the analyzer continues to the postamble rather than abandoning
the frame.

---

## Step 13 — Postamble (4 bits = 1111, control, all frames)

```cpp
// FSIAnalyzer.cpp:406
        U64 post_val, ps, pe;
        CollectBits( 4, post_val, ps, pe, false );
```

The postamble is four clock edges with data HIGH. A `FSI_RESULT_POSTAMBLE`
(0x08) bubble is emitted for it. Its only purpose is to let the transmitter
drive the lines back to idle before the next frame begins.

---

## Step 14 — Commit and loop

```cpp
// FSIAnalyzer.cpp:410
        mResults->CommitResults();
        ReportProgress( mClock->GetSampleNumber() );
    }
```

`CommitResults()` flushes all the `AddFrame()` / `AddFrameV2()` calls made
during this frame to the Logic 2 display layer. Bubble labels become visible.

`CommitPacketAndStartNewPacket()` (called at the top of the loop after
`SyncPreamble()` returns) groups all frames from one FSI frame into a single
packet, which Logic 2 shows as a collapsible unit in the data table.

The outer `while( true )` then calls `SyncPreamble()` again and the entire
state machine repeats for the next frame.

---

## Complete state diagram

```
                         ┌─────────────────────────────────────────┐
                         │              IDLE / SCANNING             │
                         │  SyncPreamble() — AdvanceToNextClockEdge │
                         │  per edge:                               │
                         │    D0=HIGH → count++, store sample       │
                         │    D0=LOW  → if count<5: reset           │
                         │             if count≥5: check SOF        │
                         └──────────────────┬──────────────────────┘
                                            │ SOF[1,2,3] = 0,0,1 confirmed
                                            ▼
                         ┌─────────────────────────────────────────┐
                         │           PREAMBLE BUBBLE EMITTED        │
                         │  Spans from preamble[0] to SOF[0]        │
                         └──────────────────┬──────────────────────┘
                                            │
                                            ▼
                         ┌─────────────────────────────────────────┐
                         │           SOF BUBBLE EMITTED             │
                         │  Spans SOF[1] to SOF[3] (0x07)          │
                         └──────────────────┬──────────────────────┘
                                            │
                                            ▼
                         ┌─────────────────────────────────────────┐
                         │   FRAME TYPE — CollectBits(4, false)     │
                         │   4 edges, D0 only                       │
                         │   Bubble: PING / ERROR / DATA(Nw) / …    │
                         └──────────────────┬──────────────────────┘
                                            │
                              IsDataFrame?  │
                   NO ◄────────────────────┤ YES
                   │                        ▼
                   │        ┌───────────────────────────────────┐
                   │        │  USER DATA — CollectBits(8, true)  │
                   │        │  Bubble: UD / UserData: 0xNN       │
                   │        └───────────────┬───────────────────┘
                   │                        │
                   │                        ▼
                   │        ┌───────────────────────────────────┐
                   │        │  DATA WORDS — CollectBits(16,true) │
                   │        │  Repeated N times                  │
                   │        │  Bubble: D0xNNNN / Data[n]: 0xNNNN │
                   │        └───────────────┬───────────────────┘
                   │                        │
                   │                        ▼
                   │        ┌───────────────────────────────────┐
                   │        │  CRC — CollectBits(8, true)        │
                   │        │  ComputeCRC() → compare            │
                   │        │  Bubble: CRC: 0xNN OK / [BAD]      │
                   │        └───────────────┬───────────────────┘
                   │                        │
                   └────────────────────────┘
                                            │
                                            ▼
                         ┌─────────────────────────────────────────┐
                         │   FRAME TAG — CollectBits(4, false)      │
                         │   All frame types                        │
                         │   Bubble: TAG / Tag: N                   │
                         └──────────────────┬──────────────────────┘
                                            │
                                            ▼
                         ┌─────────────────────────────────────────┐
                         │   EOF — CollectBits(4, false)            │
                         │   Expected 0110                          │
                         │   Match → EOF bubble                     │
                         │   Mismatch → ERR bubble                  │
                         └──────────────────┬──────────────────────┘
                                            │
                                            ▼
                         ┌─────────────────────────────────────────┐
                         │   POSTAMBLE — CollectBits(4, false)      │
                         │   Emits FSI_RESULT_POSTAMBLE (0x08)      │
                         └──────────────────┬──────────────────────┘
                                            │
                                            ▼
                         ┌─────────────────────────────────────────┐
                         │   CommitResults() — frame visible in UI  │
                         │   Loop back to IDLE / SCANNING           │
                         └─────────────────────────────────────────┘
```

---

## FSI flush sequence and analyzer behavior

### What a flush is

The FSI peripheral provides a software-triggered flush operation (TRM §31.3.5,
Figure 31-8). Its purpose is to reset the receiver and re-synchronize both
ends after a glitch or error. The sequence on the wire is:

1. **Data line pulse (no clock)** — while CLK is still idle (no clock edges),
   TXD0 and TXD1 each toggle LOW and then back HIGH. The transmitter drives
   one full LOW→HIGH cycle on each data line, then both settle back to idle
   HIGH. CLK never toggles during this step.

2. **5 full CLK cycles** — after the data lines are back HIGH, CLK runs 10
   half-period edges (falling + rising × 5). TXD0 and TXD1 remain HIGH for
   the entire duration of these clock cycles. This produces 10 consecutive HIGH
   bits in the analyzer's view.

3. **Idle gap** — CLK returns to idle (HIGH, no edges). There is a substantial
   idle gap between the flush CLK cycles and the first real preamble+frame.

4. **Normal frame** — preamble (1111) + SOF (1001) + rest of frame, as usual.

The critical point: **the data line pulse in step 1 produces no clock edges.**
The analyzer only advances sample position on clock edges
(`AdvanceToNextClockEdge`). The TXD0/TXD1 toggle during the flush is therefore
completely invisible to the analyzer — it never sees it.

### Scenario 1 — CLK starts LOW (MCU boots during capture)

The capture begins with CLK=LOW and D0=LOW (MCU not yet configured). The MCU
then boots, configuring FSI so CLK and D0 both go HIGH — producing one rising
edge on CLK with D0=LOW (the boot edge). The boot edge fires a reset inside
`SyncPreamble` (`high_count=0; r=0`). The firmware then issues a flush: D0
pulses LOW then back HIGH while CLK stays idle (invisible to the analyzer), and
then CLK runs 10 flush edges with D0=HIGH, followed by an idle gap and then the
normal preamble+frame.

Edge-by-edge trace through `SyncPreamble`:

| Event | D0 | high_count | r (before++) | Slot (r%5) | Sample stored |
|---|---|---|---|---|---|
| Boot CLK↑ | LOW | 0 (reset) | — | — | — |
| D0 flush pulse — no CLK edges | — | — | — | — | **not seen** |
| Flush CLK↓ (1 of 10) | HIGH | 1 | 0 | 0 | F1 |
| Flush CLK↑ (2 of 10) | HIGH | 2 | 1 | 1 | F2 |
| Flush CLK↓ (3 of 10) | HIGH | 3 | 2 | 2 | F3 |
| Flush CLK↑ (4 of 10) | HIGH | 4 | 3 | 3 | F4 |
| Flush CLK↓ (5 of 10) | HIGH | 5 | 4 | 4 | F5 |
| Flush CLK↑ (6 of 10) | HIGH | 6 | 5 | 0 | F6 ← overwrites F1 |
| Flush CLK↓ (7 of 10) | HIGH | 7 | 6 | 1 | F7 ← overwrites F2 |
| Flush CLK↑ (8 of 10) | HIGH | 8 | 7 | 2 | F8 |
| Flush CLK↓ (9 of 10) | HIGH | 9 | 8 | 3 | F9 |
| Flush CLK↑ (10 of 10) | HIGH | 10 | 9 | 4 | F10 |
| Idle gap — CLK stays HIGH, no edges | — | — | — | — | — |
| Preamble P1: CLK↓ | HIGH | 11 | 10 | 0 | P1 ← overwrites F6 |
| Preamble P2: CLK↑ | HIGH | 12 | 11 | 1 | P2 ← overwrites F7 |
| Preamble P3: CLK↓ | HIGH | 13 | 12 | 2 | P3 ← overwrites F8 |
| Preamble P4: CLK↑ | HIGH | 14 | 13 | 3 | P4 ← overwrites F9 |
| SOF[0]: CLK↓ | HIGH | 15 | 14 | 4 | SOF0 ← overwrites F10 |
| SOF[1]: CLK↑ | **LOW** | — | — | SOF check triggered | |

`high_count = 15 ≥ 5` → enter SOF check: b2=LOW ✓, b3=HIGH ✓. Ring state after SOF[0] (`r = 15`):

```
s_ring[0] = P1    s_ring[1] = P2    s_ring[2] = P3
s_ring[3] = P4    s_ring[4] = SOF0
```

```
pre_start = s_ring[r % 5]       = s_ring[15 % 5] = s_ring[0] = P1    ✓
pre_end   = s_ring[(r-1+5) % 5] = s_ring[19 % 5] = s_ring[4] = SOF0  ✓
```

**Result: preamble bubble spans P1 → SOF0. Correct.**

### Scenario 2 — CLK starts HIGH (MCU already running when capture starts)

The capture begins mid-idle: CLK=HIGH, D0=HIGH. The MCU then issues a flush,
followed by an idle gap and then a normal frame. There is no init clock
pre-advance (that code was removed). The first
`AdvanceToNextClockEdge` call blocks until the first actual clock edge, which
is the first falling edge of the flush CLK cycles.

Edge-by-edge trace through `SyncPreamble`:

| Event | D0 | high_count | r (before++) | Slot (r%5) | Sample stored |
|---|---|---|---|---|---|
| D0 flush pulse — no CLK edges | — | — | — | — | **not seen** |
| Flush CLK↓ (1 of 10) | HIGH | 1 | 0 | 0 | F1 |
| Flush CLK↑ (2 of 10) | HIGH | 2 | 1 | 1 | F2 |
| Flush CLK↓ (3 of 10) | HIGH | 3 | 2 | 2 | F3 |
| Flush CLK↑ (4 of 10) | HIGH | 4 | 3 | 3 | F4 |
| Flush CLK↓ (5 of 10) | HIGH | 5 | 4 | 4 | F5 |
| Flush CLK↑ (6 of 10) | HIGH | 6 | 5 | 0 | F6 ← overwrites F1 |
| Flush CLK↓ (7 of 10) | HIGH | 7 | 6 | 1 | F7 ← overwrites F2 |
| Flush CLK↑ (8 of 10) | HIGH | 8 | 7 | 2 | F8 |
| Flush CLK↓ (9 of 10) | HIGH | 9 | 8 | 3 | F9 |
| Flush CLK↑ (10 of 10) | HIGH | 10 | 9 | 4 | F10 |
| Idle gap — CLK stays HIGH, no edges | — | — | — | — | — |
| Preamble P1: CLK↓ | HIGH | 11 | 10 | 0 | P1 ← overwrites F6 |
| Preamble P2: CLK↑ | HIGH | 12 | 11 | 1 | P2 ← overwrites F7 |
| Preamble P3: CLK↓ | HIGH | 13 | 12 | 2 | P3 ← overwrites F8 |
| Preamble P4: CLK↑ | HIGH | 14 | 13 | 3 | P4 ← overwrites F9 |
| SOF[0]: CLK↓ | HIGH | 15 | 14 | 4 | SOF0 ← overwrites F10 |
| SOF[1]: CLK↑ | **LOW** | — | — | SOF check triggered | |

`high_count = 15 ≥ 5` → enter SOF check: b2=LOW ✓, b3=HIGH ✓. Ring state after SOF[0] (`r = 15`):

```
s_ring[0] = P1    s_ring[1] = P2    s_ring[2] = P3
s_ring[3] = P4    s_ring[4] = SOF0
```

```
pre_start = s_ring[r % 5]       = s_ring[15 % 5] = s_ring[0] = P1    ✓
pre_end   = s_ring[(r-1+5) % 5] = s_ring[19 % 5] = s_ring[4] = SOF0  ✓
```

**Result: preamble bubble spans P1 → SOF0. Correct.**

### Why the ring buffer absorbs any number of flush HIGHs

The ring has 5 slots. Every time `high_count` accumulates one more HIGH beyond
5, `r % 5` wraps around and overwrites the oldest entry. No matter how many
flush clock cycles appear before the preamble, the ring always ends up
containing only the **5 most recent** HIGH sample positions at the moment
SOF[1] (LOW) arrives. Those 5 most recent HIGHs are always
P1, P2, P3, P4, SOF[0] — the exact preamble + SOF[0] samples needed.

The `pre_start` formula `s_ring[r % 5]` points to the slot that will next be
overwritten — i.e., the oldest entry — which is always P1 regardless of how
many flush HIGHs preceded it.

### No code changes required

The analyzer handles the flush sequence correctly without any special logic:

- The data-line pulse has no clock edges → invisible, no state change.
- The flush CLK cycles (D0=HIGH) simply grow `high_count` above its minimum
  threshold of 5.
- The ring buffer silently discards all but the most recent 5 HIGH samples.
- The idle gap between flush CLK cycles and the preamble produces no clock
  edges → also invisible, `high_count` and `r` hold their current values.
- When the preamble begins, its 4 bits plus SOF[0] fill the ring with the
  correct boundary samples.

---

## Capture scenarios — normal frame detection

This section traces how `SyncPreamble` processes a normal FSI frame (no flush)
in each capture scenario, and confirms the analyzer produces the correct
preamble bubble and frame boundaries in both cases.

### Scenario 1 — CLK starts LOW (MCU boots during capture)

The capture begins with CLK=LOW and D0=LOW (MCU not yet configured). At some
point the MCU configures the FSI peripheral: both CLK and D0 transition to
HIGH (idle state). This produces **one rising edge on CLK** — the boot edge.
CLK then stays HIGH (idle) until the first frame begins.

```
CLK: ______↑‾‾‾‾‾‾‾‾‾‾‾‾‾‾↓↑↓↑↓↑↓↑↓↑  ← preamble + SOF
D0:  _______‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾
              boot edge
              CLK↑, D0=HIGH
```

**D0 at the boot edge is HIGH** — both CLK and D0 settle to idle HIGH together.
The boot edge is therefore counted as one HIGH bit and lands in slot 0 of the
ring buffer.

Edge-by-edge trace (`SyncPreamble`):

| Edge | Description | D0 | high_count | r (before++) | Slot | Sample stored |
|------|--------------|----|------------|--------------|------|---------------|
| 1 | Boot CLK↑ | HIGH | 1 | 0 | 0 | boot |
| — | Idle gap — no CLK edges | — | — | — | — | — |
| 2 | P1: CLK↓ | HIGH | 2 | 1 | 1 | P1 |
| 3 | P2: CLK↑ | HIGH | 3 | 2 | 2 | P2 |
| 4 | P3: CLK↓ | HIGH | 4 | 3 | 3 | P3 |
| 5 | P4: CLK↑ | HIGH | 5 | 4 | 4 | P4 |
| 6 | SOF[0]: CLK↓ | HIGH | 6 | 5 | 0 | SOF0 ← overwrites boot |
| 7 | SOF[1]: CLK↑ | **LOW** | — | — | SOF check triggered | |

At edge 7, `high_count = 6 ≥ 5`. The SOF check reads b2 (LOW ✓) and b3
(HIGH ✓) — frame confirmed.

Ring buffer state after edge 6 (`r = 6`):

```
s_ring[0] = SOF0   (boot sample overwritten at r=5)
s_ring[1] = P1
s_ring[2] = P2
s_ring[3] = P3
s_ring[4] = P4
```

Commit calculation:

```cpp
pre_start = s_ring[r % 5]          = s_ring[6 % 5]       = s_ring[1] = P1   ✓
pre_end   = s_ring[(r-1+5) % 5]    = s_ring[(5+5) % 5]   = s_ring[0] = SOF0 ✓
```

**Preamble bubble: P1 → SOF0. Correct.**

> **What if D0 is still LOW when CLK rises at boot?**
> Some MCU configurations may cause CLK to rise a few samples before D0 settles.
> If D0=LOW at the boot edge, `high_count` is reset to 0 immediately, and `r`
> resets to 0. The preamble then sees edges with `r` starting from 0 and
> `high_count` building from 1. With only 4+1=5 preamble+SOF[0] HIGHs, the ring
> fills slots 0–4 exactly with P1, P2, P3, P4, SOF0. At SOF[1]:
> `r=5`, `pre_start=s_ring[0]=P1`, `pre_end=s_ring[4]=SOF0`. Still correct.

### Scenario 2 — CLK starts HIGH (MCU already running)

The capture begins mid-idle: CLK=HIGH, D0=HIGH. There is **no boot edge** — the
MCU was already configured before the Saleae capture started. There is also no
init clock pre-advance (that code was removed; see Step 2). The first
`AdvanceToNextClockEdge` call blocks until the first real clock edge, which is
the falling edge of preamble bit P1.

```
CLK: ‾‾‾‾‾‾‾‾‾‾‾‾‾‾↓↑↓↑↓↑↓↑↓↑  ← preamble + SOF
D0:  ‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾
     ↑ capture start
       CLK=HIGH (idle)
```

Edge-by-edge trace:

| Edge | Description | D0 | high_count | r (before++) | Slot | Sample stored |
|------|--------------|----|------------|--------------|------|---------------|
| 1 | P1: CLK↓ | HIGH | 1 | 0 | 0 | P1 |
| 2 | P2: CLK↑ | HIGH | 2 | 1 | 1 | P2 |
| 3 | P3: CLK↓ | HIGH | 3 | 2 | 2 | P3 |
| 4 | P4: CLK↑ | HIGH | 4 | 3 | 3 | P4 |
| 5 | SOF[0]: CLK↓ | HIGH | 5 | 4 | 4 | SOF0 |
| 6 | SOF[1]: CLK↑ | **LOW** | — | — | SOF check triggered | |

At edge 6, `high_count = 5 ≥ 5`. SOF check: b2=LOW ✓, b3=HIGH ✓.

Ring buffer state after edge 5 (`r = 5`):

```
s_ring[0] = P1
s_ring[1] = P2
s_ring[2] = P3
s_ring[3] = P4
s_ring[4] = SOF0
```

Commit calculation:

```cpp
pre_start = s_ring[r % 5]          = s_ring[5 % 5]       = s_ring[0] = P1   ✓
pre_end   = s_ring[(r-1+5) % 5]    = s_ring[(4+5) % 5]   = s_ring[4] = SOF0 ✓
```

**Preamble bubble: P1 → SOF0. Correct.**

The first frame is decoded identically to Scenario 1 — the ring buffer happens
to be perfectly sized for the minimum case.

### Subsequent frames (both scenarios)

After each frame is decoded, `WorkerThread` calls `SyncPreamble()` again. At
that point the postamble (4 HIGH bits on D0) has just been consumed by
`CollectBits(4, false)`, and CLK has returned to idle (HIGH, no edges). The
state machine is in the same position as Scenario 2 above: waiting for the
next clock edge on an idle CLK=HIGH line.

Every subsequent frame is therefore identical to the Scenario 2 trace:

| Edge | Description | D0 | high_count | r (before++) | Slot | Sample |
|------|--------------|----|------------|--------------|------|--------|
| 1 | P1: CLK↓ | HIGH | 1 | 0 | 0 | P1 |
| 2 | P2: CLK↑ | HIGH | 2 | 1 | 1 | P2 |
| 3 | P3: CLK↓ | HIGH | 3 | 2 | 2 | P3 |
| 4 | P4: CLK↑ | HIGH | 4 | 3 | 3 | P4 |
| 5 | SOF[0]: CLK↓ | HIGH | 5 | 4 | 4 | SOF0 |
| 6 | SOF[1]: CLK↑ | **LOW** | — | — | SOF check | |

`high_count=5`, `r=5`. `pre_start=s_ring[0]=P1`, `pre_end=s_ring[4]=SOF0`. ✓

`SyncPreamble` is stateless between calls (`high_count` and `s_ring` are local
variables, re-initialized to zero on every entry), so no state leaks between
frames.

### Summary

| Capture scenario | Boot edge | high_count at SOF[1] | pre_start | pre_end |
|---|---|---|---|---|
| Scenario 1 — CLK=LOW at start, D0=HIGH at boot edge | Counted as 1 HIGH (overwritten by SOF0) | 6 | P1 ✓ | SOF0 ✓ |
| Scenario 1 — CLK=LOW at start, D0=LOW at boot edge | Resets counter; preamble fills ring from 0 | 5 | P1 ✓ | SOF0 ✓ |
| Scenario 2 — CLK=HIGH at start (first frame) | None | 5 | P1 ✓ | SOF0 ✓ |
| Any scenario — subsequent frames | None (postamble consumed, CLK idle) | 5 | P1 ✓ | SOF0 ✓ |

The ring buffer correctly resolves every case:

- **Extra leading HIGH** (boot edge, flush HIGHs): overwritten by SOF0 landing in
  slot 0 before `pre_start` is computed.
- **Exact minimum** (scenario 2, subsequent frames): ring fills slots 0–4
  with P1..SOF0 precisely; `r%5=0` points directly at P1.
- **No special-case code needed** for any scenario: one ring-buffer overwrite
  mechanism handles all of them.

---

## Frame formats and bit-level detail

This section describes every FSI field at the bit level, maps each field to
the code that reads it, and shows the exact wire sequence for each frame type.

---

### Idle state

```
CLK: ‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾  (no edges — clock stopped)
D0:  ‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾  (HIGH)
D1:  ‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾  (HIGH, 2-lane only)
```

The FSI clock only runs during frame transmission. Between frames — and before
the very first frame — CLK is HIGH with no toggling. D0 and D1 are also HIGH.
The analyzer sits blocked inside `AdvanceToNextClockEdge` → `mClock->AdvanceToNextEdge()`
until the first falling edge of the preamble.

---

### Complete field map

Every FSI frame contains these fields in order. Whether a field is present
depends on the frame type (see matrix below).

| # | Field | Bits | Code | 2-lane handling | Expected value / range |
|---|-------|------|------|-----------------|------------------------|
| 1 | Preamble | 4 | `SyncPreamble` | both lanes HIGH | 1111 |
| 2 | SOF | 4 | `SyncPreamble` (consumed) | both lanes | 1001 |
| 3 | Frame Type | 4 | `CollectBits(4, …, false)` | D0 only | see type codes |
| 4 | User Data | 8 | `CollectBits(8, …, true)` | interleaved | 0x00–0xFF |
| 5 | Data Words | N×16 | `CollectBits(16, …, true)` × N | interleaved | 0x0000–0xFFFF each |
| 6 | CRC | 8 | `CollectBits(8, …, true)` | interleaved | CRC-8 of fields 4+5 |
| 7 | Frame Tag | 4 | `CollectBits(4, …, false)` | D0 only | 0x0–0xF |
| 8 | EOF | 4 | `CollectBits(4, …, false)` | D0 only | 0110 (0x6) |
| 9 | Postamble | 4 | `CollectBits(4, …, false)` (silent) | both lanes HIGH | 1111 |

Fields 1–2 are detected by `SyncPreamble`. Fields 3–9 are collected by
`WorkerThread`. Fields 4–6 are present only in data frames (see
`IsDataFrame()`). Field 9 is consumed but not emitted as a Saleae frame.

---

### Frame type codes

Defined in `FSIAnalyzer.h` lines 10–16 and decoded by `FrameTypeName()` at
line 429.

| Constant | Hex | Binary | Wire label | Data fields? | Word count |
|---|---|---|---|---|---|
| `FSI_FRAME_TYPE_PING`  | 0x0 | 0000 | PING      | No  | 0 |
| `FSI_FRAME_TYPE_NWORD` | 0x3 | 0011 | DATA(Nw)  | Yes | N (settings) |
| `FSI_FRAME_TYPE_DATA1` | 0x4 | 0100 | DATA(1w)  | Yes | 1 |
| `FSI_FRAME_TYPE_DATA2` | 0x5 | 0101 | DATA(2w)  | Yes | 2 |
| `FSI_FRAME_TYPE_DATA4` | 0x6 | 0110 | DATA(4w)  | Yes | 4 |
| `FSI_FRAME_TYPE_DATA6` | 0x7 | 0111 | DATA(6w)  | Yes | 6 |
| `FSI_FRAME_TYPE_ERROR` | 0xF | 1111 | ERROR     | No  | 0 |

All other 4-bit codes are reserved. `DataWordCount()` (line 62) returns the
word count for each type; it returns 0 for PING, ERROR, and unknown codes.
`IsDataFrame()` (line 76) returns true for NWORD, DATA1, DATA2, DATA4, DATA6.

---

### Field-by-field bit detail

#### Preamble (4 bits)

```
D0: 1  1  1  1
    ↑  ↑  ↑  ↑
    P1 P2 P3 P4   (all HIGH, clock running, indistinguishable from extended idle)
```

Four consecutive HIGH bits on every clock edge. Since idle is also HIGH,
the preamble is not inherently distinguishable from idle — it is the SOF
pattern immediately following that proves a frame has started.

The preamble is not directly collected by `CollectBits`. `SyncPreamble` scans
for it implicitly by counting consecutive HIGH bits until the count reaches
≥ 5, then checking the next three bits for the SOF[1,2,3] = 0,0,1 pattern.

**Saleae result:** `FSI_RESULT_PREAMBLE` (0x00). `mData1=0, mData2=0,
mFlags=0`. Bubble spans from P1 sample to SOF[0] sample. SOF[1,2,3] are
emitted as a separate `FSI_RESULT_SOF` (0x07) bubble spanning SOF[1] to
SOF[3].

---

#### SOF — Start of Frame (4 bits)

```
D0: 1  0  0  1
    ↑  ↑  ↑  ↑
   [0][1][2][3]   SOF bit indices
```

The fixed pattern 1001 immediately follows the preamble HIGHs. The analyzer
detects it as part of `SyncPreamble`:

- SOF[0] = 1 (HIGH): counted as the 5th (or more) consecutive HIGH in the
  ring buffer. This is the last sample stored in the ring and becomes `pre_end`.
- SOF[1] = 0 (LOW): triggers the SOF check branch (`high_count >= 5`).
- SOF[2] = 0 (LOW): read as `b2`, confirmed LOW.
- SOF[3] = 1 (HIGH): read as `b3`, confirmed HIGH. Frame start confirmed.
  `frame_start_sample = s3` (the sample number of SOF[3]).

**Saleae result:** `FSI_RESULT_SOF` (0x07). `mData1=0, mData2=0, mFlags=0`.
Bubble spans from SOF[1] sample to SOF[3] sample. Bubble label: `"SOF"`
(short) or `"Start of Frame"` (expanded). Emitted immediately after the
preamble bubble inside `SyncPreamble`.

---

#### Frame Type (4 bits) — control field

```
Code example — DATA(1w) = 0x4 = 0100:
D0: 0  1  0  0
    ↑  ↑  ↑  ↑
   b3 b2 b1 b0   (MSB first)
```

Collected by `CollectBits(4, ft_val, ft_start, ft_end, false)` at line 300.
`interleaved=false`: in 2-lane mode D0 and D1 both carry the full identical
4-bit field; the analyzer reads D0 only.

The 4 bits are shifted in MSB-first: each edge shifts `value` left 1 and ORs
the D0 bit into bit 0. After 4 edges `value` holds the complete 4-bit code.

**Saleae result:** `FSI_RESULT_FRAME_TYPE` (0x01). `mData1 = frame_type (0x0–0xF)`.
FrameV2 adds `"type"` string (e.g. `"DATA(1w)"`) and `"value"` integer.

---

#### User Data (8 bits) — data frames only, data field

```
1-lane example — value 0xA5 = 10100101:
D0: 1  0  1  0  0  1  0  1
    ↑  ↑  ↑  ↑  ↑  ↑  ↑  ↑
   b7 b6 b5 b4 b3 b2 b1 b0   (MSB first, 8 edges from D0)

2-lane example — same value 0xA5:
       Edge 0       Edge 1       Edge 2       Edge 3
D0:    1   (b7)     1   (b5)     0   (b3)     0   (b1)
D1:    0   (b6)     0   (b4)     1   (b2)     1   (b0)
```

Collected by `CollectBits(8, ud_val, ud_start, ud_end, true)` at line 324.

- **1-lane** (`interleaved=true`, `mTwoLane=false`): falls through to the
  1-lane branch (line 147), reads 8 edges from D0, shifts MSB-first.
- **2-lane** (`interleaved=true`, `mTwoLane=true`): takes the 2-lane branch
  (line 125). `edges = (8+1)/2 = 4`. Each of the 4 edges contributes
  `D0 → even-position bit` then `D1 → odd-position bit`, both shifted left
  into `value`. After 4 edges: `value = D0[e0] D1[e0] D0[e1] D1[e1] D0[e2]
  D1[e2] D0[e3] D1[e3]` = bits 7,6,5,4,3,2,1,0.

User Data is pushed first into `crc_buf` (line 326) before data words.

**Saleae result:** `FSI_RESULT_USERDATA` (0x03). `mData1 = user_data (0x00–0xFF)`.

---

#### Data Words (16 bits each, N words) — data frames only, data field

```
1-lane example — word value 0x1234:
D0: 0 0 0 1 0 0 1 0 0 0 1 1 0 1 0 0
   b15 ...                        b0  (MSB first, 16 edges)

2-lane example — same value 0x1234 = 0001 0010 0011 0100:
       E0     E1     E2     E3     E4     E5     E6     E7
D0:  0(b15) 0(b13) 1(b11) 0(b9) 0(b7) 1(b5) 0(b3) 0(b1)
D1:  0(b14) 1(b12) 0(b10) 0(b8) 0(b6) 1(b4) 1(b2) 0(b0)
```

Collected by `CollectBits(16, word_val, ws, we, true)` at line 345, once per
word in a loop (`for w = 0 .. num_words-1`).

- **2-lane**: `edges = (16+1)/2 = 8` (integer division rounds down to 8,
  which is exactly 16/2). All 16 bits covered, no partial edge.
- Word index `w` (zero-based) is stored in `mData2` and shown in the bubble
  label as `Data[n]`.

Each word is fed into `crc_buf` **LSB first then MSB** (lines 346–347):

```cpp
crc_buf.push_back( (U8)( word_val      ) );   // bits [7:0]  — LSB
crc_buf.push_back( (U8)( word_val >> 8 ) );   // bits [15:8] — MSB
```

This byte order matches the FSI TRM CRC specification (little-endian per
word for CRC purposes, regardless of the MSB-first wire order).

**Saleae result:** `FSI_RESULT_DATA_WORD` (0x04). `mData1 = word_val (16-bit)`,
`mData2 = word_index (0-based)`, `mFlags = 0`.

---

#### CRC (8 bits) — data frames only, data field

```
1-lane: 8 edges from D0, MSB first.
2-lane: 4 edges, interleaved same as User Data.
```

Collected by `CollectBits(8, crc_val, cs, ce, true)` at line 362. After
collection, `ComputeCRC(crc_buf)` (line 363) computes the expected CRC over
the bytes accumulated in `crc_buf` and compares:

```cpp
bool crc_ok = ( (U8)crc_val == computed_crc );   // line 364
```

**CRC algorithm:** CRC-8, polynomial `x^8 + x^2 + x + 1` (0x07), seed
`0x00`, no final XOR. Implemented as a 256-entry lookup table `kFsiCrcTable`.

**CRC input byte order:**
1. User Data byte (1 byte)
2. Word 0: byte[0] = bits[7:0] (LSB), byte[1] = bits[15:8] (MSB)
3. Word 1: byte[0] = bits[7:0], byte[1] = bits[15:8]
4. … (one pair per data word)

Frame Type and Frame Tag are **not** included in the CRC.

**Saleae result:** `FSI_RESULT_CRC` (0x05).

| Field | Contains |
|---|---|
| `mData1` | Received CRC byte from wire (displayed in bubble) |
| `mData2` | Computed (expected) CRC value |
| `mFlags` | Bit 0 = 1 if CRC matched, 0 if CRC failed |

FrameV2 adds `"received"`, `"expected"`, and `"status"` (`"OK"` or `"FAIL"`).
The bubble shows `CRC: 0xNN OK` or `CRC: 0xNN [BAD]`.

---

#### Frame Tag (4 bits) — control field

```
Example — tag value 0x5 = 0101:
D0: 0  1  0  1
    ↑  ↑  ↑  ↑
   b3 b2 b1 b0   (MSB first)
```

A user-defined 4-bit identifier (0x0–0xF). Firmware sets this to distinguish
between frame sources or message types. The analyzer does not interpret its
value — it is passed through as-is.

Collected by `CollectBits(4, tag_val, tag_start, tag_end, false)` at line 381.
`interleaved=false`: D0 only in both 1-lane and 2-lane mode.

Present in **all** frame types including PING and ERROR. Collected after the
data fields block (or immediately after Frame Type for PING/ERROR).

**Saleae result:** `FSI_RESULT_TAG` (0x02). `mData1 = tag_val (0x0–0xF)`.

---

#### EOF — End of Frame (4 bits) — control field

```
Fixed pattern 0110:
D0: 0  1  1  0
    ↑  ↑  ↑  ↑
   b3 b2 b1 b0
```

Collected by `CollectBits(4, eof_val, es, ee, false)` at line 397. The value
is compared against `0x6` (binary 0110) at line 398:

```cpp
bool eof_ok = ( eof_val == 0x6 );   // 0110
```

`interleaved=false`: D0 only. In 2-lane mode D1 carries the same pattern but
is not read.

**Saleae result:**
- Match: `FSI_RESULT_EOF` (0x06). `mData1 = 0x6`.
- Mismatch: `FSI_RESULT_ERROR` (0xFF). `mData1 = received_value`.

In both cases `mData2 = 0`, `mFlags = 0`.

---

#### Postamble (4 bits) — control field

```
Fixed pattern 1111 (all HIGH, mirrors preamble):
D0: 1  1  1  1
```

Collected by `CollectBits(4, post_val, ps, pe, false)` at line 413. The
collected value is not validated.

**Saleae result:** `FSI_RESULT_POSTAMBLE` (0x08). `mData1=0, mData2=0,
mFlags=0`. Bubble label: `"POST"` (short) or `"Postamble"` (expanded).
The postamble's purpose is to drive lines back to idle HIGH before CLK
stops. After this collection, `CommitResults()` is called and the loop
returns to `SyncPreamble` for the next frame.

---

### Frame type presence matrix

Which fields appear on the wire for each frame type:

| Field | PING (0000) | ERROR (1111) | DATA(1w) (0100) | DATA(2w) (0101) | DATA(4w) (0110) | DATA(6w) (0111) | DATA(Nw) (0011) |
|---|:---:|:---:|:---:|:---:|:---:|:---:|:---:|
| Preamble + SOF | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| Frame Type | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| User Data | — | — | ✓ | ✓ | ✓ | ✓ | ✓ |
| Data Words | — | — | 1 word | 2 words | 4 words | 6 words | N words |
| CRC | — | — | ✓ | ✓ | ✓ | ✓ | ✓ |
| Frame Tag | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| EOF | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| Postamble | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |

---

### Complete wire bit streams

One bit per clock edge, MSB first within each field.

#### PING frame (no data, 1-lane)

```
Total bits: 4+4+4+4+4+4 = 24 clock edges

 ← preamble → ← SOF → ← FT  → ← TAG  → ← EOF  → ← post →
  1  1  1  1   1  0  0  1   0  0  0  0   t  t  t  t   0  1  1  0   1  1  1  1
 [P1][P2][P3][P4][S0][S1][S2][S3][f3][f2][f1][f0][g3][g2][g1][g0][e3][e2][e1][e0][q3][q2][q1][q0]

S1,S2,S3 = SOF[1,2,3] emitted as SOF bubble (FSI_RESULT_SOF, 0x07)
FT = 0000 (PING)
TAG = user-defined 4-bit value (shown as t)
EOF = 0110
```

Saleae bubbles emitted in order: `PRE | SOF | PING | TAG | EOF | POST`

---

#### ERROR frame (no data, 1-lane)

```
Total bits: 4+4+4+4+4+4 = 24 clock edges

 ← preamble → ← SOF → ← FT  → ← TAG  → ← EOF  → ← post →
  1  1  1  1   1  0  0  1   1  1  1  1   t  t  t  t   0  1  1  0   1  1  1  1
                            [f3][f2][f1][f0]
                             FT = 1111 (ERROR)
```

Saleae bubbles: `PRE | SOF | ERROR | TAG | EOF | POST`

---

#### DATA(1w) frame (1 data word, 1-lane)

```
Total bits: 4+4 + 4 + 8 + 16 + 8 + 4 + 4 + 4 = 56 clock edges

 ←pre→ ←SOF→  ←FT→   ←── user data (8) ──→  ←────── data word 0 (16) ──────→  ←── CRC (8) ──→  ←TAG→  ←EOF→  ←post→
  1111  1001   0100   ud7 ud6 ud5 ud4 ud3 ud2 ud1 ud0   w15 w14 w13 ... w1 w0   c7 c6 c5 c4 c3 c2 c1 c0   tttt   0110   1111
```

Saleae bubbles: `PRE | SOF | DATA(1w) | UD | D[0] | CRC | TAG | EOF | POST`

---

#### DATA(2w) frame (2 data words, 2-lane)

In 2-lane mode, data fields (User Data, Data Words, CRC) each consume half as
many clock edges because each edge delivers two bits. Control fields (FT, TAG,
EOF, Postamble) still consume 4 edges from D0 only.

```
Total clock edges: 4+4 + 4 + 4 + 8 + 4 + 4 + 4 + 4 = 40 edges
  (preamble+SOF: 8, FT: 4, UD: 4 edges×2bits, Word0: 8 edges×2bits,
   Word1: 8 edges×2bits, CRC: 4 edges×2bits, TAG: 4, EOF: 4, Post: 4)

Frame Type (4 edges, D0 only):
  CLK edge:  1     2     3     4
  D0:        f3    f2    f1    f0     ← FT = 0101 for DATA(2w)
  D1:        f3    f2    f1    f0     (identical, not read)

User Data (4 edges, interleaved):
  CLK edge:  1     2     3     4
  D0:        ud7   ud5   ud3   ud1   ← even-position bits
  D1:        ud6   ud4   ud2   ud0   ← odd-position bits
  Assembled: ud7 ud6  ud5 ud4  ud3 ud2  ud1 ud0  = 0xNN

Data Word 0 (8 edges, interleaved):
  CLK edge:  1     2     3     4     5     6     7     8
  D0:       w15   w13   w11    w9    w7    w5    w3    w1
  D1:       w14   w12   w10    w8    w6    w4    w2    w0

Data Word 1 (8 edges, interleaved) — same pattern as word 0.

CRC (4 edges, interleaved):
  CLK edge:  1     2     3     4
  D0:        c7    c5    c3    c1
  D1:        c6    c4    c2    c0
```

Saleae bubbles: `PRE | SOF | DATA(2w) | UD | D[0] | D[1] | CRC | TAG | EOF | POST`

---

### Saleae result frame storage summary

All fields stored using `mResults->AddFrame()` (legacy V1) and
`mResults->AddFrameV2()` simultaneously.

| FSI_RESULT_* | mType | mData1 | mData2 | mFlags |
|---|---|---|---|---|
| PREAMBLE    (0x00) | 0x00 | 0 | 0 | 0 |
| FRAME_TYPE  (0x01) | 0x01 | frame type code (0x0–0xF) | 0 | 0 |
| TAG         (0x02) | 0x02 | tag value (0x0–0xF) | 0 | 0 |
| USERDATA    (0x03) | 0x03 | user data byte (0x00–0xFF) | 0 | 0 |
| DATA_WORD   (0x04) | 0x04 | 16-bit word value | word index (0-based) | 0 |
| CRC         (0x05) | 0x05 | received CRC byte | computed CRC byte | bit 0: 1=OK 0=FAIL |
| EOF         (0x06) | 0x06 | 0x6 | 0 | 0 |
| SOF         (0x07) | 0x07 | 0 | 0 | 0 |
| POSTAMBLE   (0x08) | 0x08 | 0 | 0 | 0 |
| ERROR       (0xFF) | 0xFF | received EOF value (≠ 0x6) | 0 | 0 |
