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

                        frame_start_sample = s3;
                        return true;
```

- `pre_start = s_ring[r % 5]` — the ring slot written 5 HIGHs ago, which is
  preamble bit 1 (the first of the four preamble HIGH edges).
- `pre_end = s_ring[(r-1+5) % 5]` — the ring slot written 1 HIGH ago, which
  is SOF[0] (the HIGH that opens the SOF pattern and is visually
  indistinguishable from the preamble on the wire).

The bubble therefore covers exactly the 4 preamble HIGHs + SOF[0].
SOF[1,2,3] (`0`, `0`, `1`) were already consumed during the peek and are
silently discarded — they appear as unlabeled bits between the PRE bubble
and the FT bubble in the Logic 2 waveform view.

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

The postamble is four clock edges with data HIGH. It is consumed silently —
no frame or bubble is emitted for it. Its only purpose is to let the
transmitter drive the lines back to idle before the next frame begins.

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
                         │   Consumed silently (no bubble)          │
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

The capture begins with CLK=LOW and D0=LOW (MCU not yet configured).

```
Time →
CLK: ____|‾|_|‾|_|‾|_|‾|_|‾|___|‾‾‾‾‾‾‾‾‾‾‾‾‾|_|‾|_|‾|_|‾|_|‾|_|‾|_|‾|_|‾|_|‾|_|...
D0:  ______|‾|__|‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾|‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾...
      ↑boot ↑flush data pulse      ↑flush 10 CLK edges  ↑idle    ↑preamble starts
      edge   (no CLK edges)         (D0=HIGH throughout)  gap
```

Edge-by-edge trace through `SyncPreamble`:

| Edge # | CLK edge | D0 | high_count | r | s_ring[r%5] written |
|--------|----------|----|------------|---|---------------------|
| 1 | boot: CLK↑ | LOW | 0 (reset) | 0 | — |
| 2 | flush D0 pulse: no CLK edges — **not seen** | — | — | — | — |
| 3 | flush CLK↓ (edge 1 of 10) | HIGH | 1 | 1 | s_ring[1] = F1 |
| 4 | flush CLK↑ (edge 2 of 10) | HIGH | 2 | 2 | s_ring[2] = F2 |
| 5 | flush CLK↓ (edge 3 of 10) | HIGH | 3 | 3 | s_ring[3] = F3 |
| 6 | flush CLK↑ (edge 4 of 10) | HIGH | 4 | 4 | s_ring[4] = F4 |
| 7 | flush CLK↓ (edge 5 of 10) | HIGH | 5 | 5 | s_ring[0] = F5 |
| 8 | flush CLK↑ (edge 6 of 10) | HIGH | 6 | 6 | s_ring[1] = F6 |
| 9 | flush CLK↓ (edge 7 of 10) | HIGH | 7 | 7 | s_ring[2] = F7 |
| 10 | flush CLK↑ (edge 8 of 10) | HIGH | 8 | 8 | s_ring[3] = F8 |
| 11 | flush CLK↓ (edge 9 of 10) | HIGH | 9 | 9 | s_ring[4] = F9 |
| 12 | flush CLK↑ (edge 10 of 10) | HIGH | 10 | 10 | s_ring[0] = F10 |
| — | idle gap — CLK stays HIGH, no edges | — | — | — | — |
| 13 | preamble P1: CLK↓ | HIGH | 11 | 11 | s_ring[1] = P1 |
| 14 | preamble P2: CLK↑ | HIGH | 12 | 12 | s_ring[2] = P2 |
| 15 | preamble P3: CLK↓ | HIGH | 13 | 13 | s_ring[3] = P3 |
| 16 | preamble P4: CLK↑ | HIGH | 14 | 14 | s_ring[4] = P4 |
| 17 | SOF[0]: CLK↓ | HIGH | 15 | 15 | s_ring[0] = SOF0 |
| 18 | SOF[1]: CLK↑ | **LOW** | reset | — | — |

At edge 18, `high_count=15 ≥ 5` so we enter the SOF check:

- Read b2 (SOF[2]): LOW ✓
- Read b3 (SOF[3]): HIGH ✓ → SOF confirmed.

Ring buffer state at commit:
```
r = 15 (after 15 HIGHs)
s_ring[0] = SOF0,  s_ring[1] = P1,  s_ring[2] = P2
s_ring[3] = P3,    s_ring[4] = P4

pre_start = s_ring[r % 5]     = s_ring[15 % 5] = s_ring[0] = SOF0
```

Wait — that gives SOF0 as pre_start. Let me re-check the code:

```cpp
U64 pre_start = ( r >= 5 ) ? s_ring[ r % 5 ] : s_ring[ 0 ];
```

With `r=15`: `s_ring[15%5] = s_ring[0] = SOF0`.  But `r` was
incremented AFTER writing to `s_ring[r%5]`, so `s_ring[r%5]` at
the time of the commit is the slot that is **about to be overwritten**
— it currently holds the value written when `r` was a multiple of 5,
which is the oldest entry still in the ring.

Stepping through the overwrites:

| r (before increment) | slot written | value |
|---|---|---|
| 0 | s_ring[0] | boot LOW — not stored (D0=LOW, so no write) |
| 1..5 | s_ring[1..0] | F1..F5 |
| 6..10 | s_ring[1..0] | F6..F10 overwrite F1..F5 |
| 11..15 | s_ring[1..0] | P1..SOF0 overwrite F6..F10 |

After edge 17 (SOF[0], r incremented to 15), the ring contains:

```
s_ring[0] = SOF0  (written at r=14, slot 14%5=4? No wait...)
```

Let me redo this carefully. The code is:

```cpp
s_ring[ r % 5 ] = s;
r++;
```

So `r` is the value **before** the increment when the sample is stored.

| D0=HIGH edge # | r (before++) | slot | sample stored |
|---|---|---|---|
| 1 (F1) | 0 | 0 | F1 |
| 2 (F2) | 1 | 1 | F2 |
| 3 (F3) | 2 | 2 | F3 |
| 4 (F4) | 3 | 3 | F4 |
| 5 (F5) | 4 | 4 | F5 |
| 6 (F6) | 5 | 0 | F6  ← overwrites F1 |
| 7 (F7) | 6 | 1 | F7  ← overwrites F2 |
| 8 (F8) | 7 | 2 | F8 |
| 9 (F9) | 8 | 3 | F9 |
| 10 (F10)| 9 | 4 | F10|
| 11 (P1) | 10| 0 | P1  ← overwrites F6 |
| 12 (P2) | 11| 1 | P2 |
| 13 (P3) | 12| 2 | P3 |
| 14 (P4) | 13| 3 | P4 |
| 15 (SOF0)| 14| 4 | SOF0|

After edge 17, `r = 15`. Ring contents:

```
s_ring[0]=P1, s_ring[1]=P2, s_ring[2]=P3, s_ring[3]=P4, s_ring[4]=SOF0
```

At the SOF check commit:
```
pre_start = s_ring[r % 5] = s_ring[15 % 5] = s_ring[0] = P1  ✓
pre_end   = s_ring[(r-1+5) % 5] = s_ring[(14+5) % 5] = s_ring[19 % 5] = s_ring[4] = SOF0  ✓
```

**Result: preamble bubble spans P1 → SOF0. Correct.**

### Scenario 2 — CLK starts HIGH (MCU already running when capture starts)

The capture begins mid-idle: CLK=HIGH, D0=HIGH. The MCU then issues a flush,
followed by an idle gap and then a normal frame.

```
Time →
CLK: ‾‾‾‾‾‾‾|_|‾|_|‾|_|‾|_|‾|_|‾|___|‾‾‾‾‾‾‾‾‾‾‾‾‾|_|‾|_|‾|_|‾|_|‾|_|‾|_|‾|_|‾|_|‾|_|...
D0:  ‾‾‾‾‾‾‾‾‾‾|_|‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾|‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾...
      ↑capture  ↑flush data pulse      ↑flush 10 CLK edges  ↑idle    ↑preamble starts
      starts     (no CLK edges)         (D0=HIGH throughout)  gap
      CLK=HIGH
```

There is no init clock pre-advance (that code was removed). The first
`AdvanceToNextClockEdge` call blocks until the first actual clock edge, which
is the first falling edge of the flush CLK cycles.

Edge-by-edge trace:

| Edge # | CLK edge | D0 | high_count | r (before++) | slot | sample |
|--------|----------|----|------------|---|---|---|
| — | D0 pulse (no CLK) | — | — | — | — | — |
| 1 | flush CLK↓ (edge 1 of 10) | HIGH | 1 | 0 | 0 | F1 |
| 2 | flush CLK↑ (edge 2 of 10) | HIGH | 2 | 1 | 1 | F2 |
| 3 | flush CLK↓ (edge 3 of 10) | HIGH | 3 | 2 | 2 | F3 |
| 4 | flush CLK↑ (edge 4 of 10) | HIGH | 4 | 3 | 3 | F4 |
| 5 | flush CLK↓ (edge 5 of 10) | HIGH | 5 | 4 | 4 | F5 |
| 6 | flush CLK↑ (edge 6 of 10) | HIGH | 6 | 5 | 0 | F6 |
| 7 | flush CLK↓ (edge 7 of 10) | HIGH | 7 | 6 | 1 | F7 |
| 8 | flush CLK↑ (edge 8 of 10) | HIGH | 8 | 7 | 2 | F8 |
| 9 | flush CLK↓ (edge 9 of 10) | HIGH | 9 | 8 | 3 | F9 |
| 10 | flush CLK↑ (edge 10 of 10) | HIGH | 10 | 9 | 4 | F10 |
| — | idle gap | — | — | — | — | — |
| 11 | preamble P1: CLK↓ | HIGH | 11 | 10 | 0 | P1 |
| 12 | preamble P2: CLK↑ | HIGH | 12 | 11 | 1 | P2 |
| 13 | preamble P3: CLK↓ | HIGH | 13 | 12 | 2 | P3 |
| 14 | preamble P4: CLK↑ | HIGH | 14 | 13 | 3 | P4 |
| 15 | SOF[0]: CLK↓ | HIGH | 15 | 14 | 4 | SOF0 |
| 16 | SOF[1]: CLK↑ | **LOW** | reset | — | — | — |

`high_count = 15 ≥ 5` → enter SOF check. Ring state after edge 15 (`r = 15`):

```
s_ring[0]=P1, s_ring[1]=P2, s_ring[2]=P3, s_ring[3]=P4, s_ring[4]=SOF0
```

```
pre_start = s_ring[15 % 5] = s_ring[0] = P1   ✓
pre_end   = s_ring[(14+5) % 5] = s_ring[4] = SOF0  ✓
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
