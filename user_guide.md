# TI FSI Analyzer — User Guide

**Author:** Nima Eskandari

## What this tool does

The TI FSI analyzer decodes **Fast Serial Interface** frames produced
by Texas Instruments FSI-capable microcontrollers directly inside
**Saleae Logic 2**.

After adding the analyzer to a capture, every FSI frame is automatically
broken into labelled segments on the waveform: preamble, frame type, tag,
user data, data words, CRC status, and end-of-frame. No manual bit-counting
or spreadsheet work is needed.

-----

## Supported frame types

| Frame type | 4-bit code | Description |
|---|---|---|
|PING      |0000|Heartbeat frame — no data words|
|ERROR     |1111|Error signalling frame — no data words|
|DATA(1w)  |0100|1 × 16-bit data word|
|DATA(2w)  |0101|2 × 16-bit data words|
|DATA(4w)  |0110|4 × 16-bit data words|
|DATA(6w)  |0111|6 × 16-bit data words|
|DATA(Nw)  |0011|N × 16-bit data words, where N is configured in the analyzer settings|

-----

## Hardware requirements

### Logic analyzer

All Saleae devices are potentially usable. The required sample rate depends
on how fast your FSI clock is configured — FSI does not have to run at its
maximum speed.

The analyzer enforces a hard minimum of **4 MS/s** (set by
`GetMinimumSampleRateHz()`). In practice, reliable edge detection needs
roughly **4–8× oversampling** of the FSI clock. Use the table below to
find the maximum FSI clock speed each device can handle:

| Device | Max digital sample rate | Max FSI clock (4× rule) |
|---|---|---|
|Logic 4|12 MS/s (all 4 channels)|~3 MHz|
|Logic 8|24 MS/s|~6 MHz|
|Logic Pro 8|500 MS/s|up to 50 MHz (FSI hardware max)|
|Logic Pro 16|500 MS/s|up to 50 MHz (FSI hardware max)|

**Examples:**
- FSI at 1 MHz clock → any device works at its default sample rate.
- FSI at 10 MHz clock → Logic Pro 8/16 required (needs ≥ 40 MS/s).
- FSI at 50 MHz clock → Logic Pro 8/16 at ≥ 200 MS/s recommended.

If you are unsure of your FSI clock frequency, check
`FSI_TX_CLKDIV` / `FSI_TX_PRESCALE` in your firmware or measure the
TXCLK period in a Logic 2 timing view before adding the analyzer.

### Probing

| FSI signal | Connect to |
|---|---|
|TXCLK / RXCLK|Any Logic channel — assign to TXCLK in settings|
|TXD0 / RXD0|Any Logic channel — assign to TXD0/RXD0 in settings|
|TXD1 / RXD1|Any Logic channel — assign to TXD1/RXD1 (2-lane only)|

If your board uses **LVDS transceivers or digital isolators**, probe the
**CMOS side** of the transceiver, not the differential pair. Logic inputs
are single-ended and will not work directly on LVDS signals.

Ground the Logic ground clip to the board ground close to the FSI device.

-----

## Installation

1. Download or build `FSIAnalyzer.so` (Linux), `FSIAnalyzer.dylib` (macOS),
   or `FSIAnalyzer.dll` (Windows). See the developer guide for build
   instructions if you need to compile from source.
2. Open **Logic 2**.
3. Go to **Edit → Settings** and scroll to **Custom Low Level Analyzers**.
4. Click **+** and point it at the folder containing the plugin file.
5. **Restart Logic 2**.

The analyzer will appear as **TI FSI** when you add a new analyzer
to a capture.

-----

## Adding the analyzer to a capture

1. Take a capture that includes TXCLK and at least TXD0.
2. In the Analyzers panel, click **+**.
3. Search for **TI FSI** and select it.
4. Configure the settings (see below) and click **Save**.

The analyzer will run immediately on the captured data. Bubble labels appear
above each channel. On large captures this may take a few seconds.

-----

## Settings reference

### Channel assignments

| Setting | Description |
|---|---|
|**TXCLK / RXCLK**|The FSI clock channel. Required.|
|**TXD0 / RXD0**|FSI data lane 0. Required.|
|**TXD1 / RXD1**|FSI data lane 1. Only needed when 2-Lane Mode is enabled.|

Assign each setting to the Logic channel you have wired to the corresponding
FSI pin. If TXD1 / RXD1 is not assigned and 2-Lane Mode is off, it is ignored.

### Protocol options

#### 2-Lane Mode

Enable this when your firmware configures the FSI peripheral for dual-lane
operation. In 2-lane mode:

- **Control fields** (Frame Type, Frame Tag, EOF, Postamble) are transmitted
  identically and completely on both TXD0 and TXD1. The analyzer reads TXD0.
- **Data fields** (User Data, Data Words, CRC) are split across the two lanes:
  TXD0 carries the even-index bits and TXD1 carries the odd-index bits,
  sampled on the same clock edge. This doubles throughput but requires both
  lines to be probed.

Leave this **off** for standard single-lane captures.

#### N-Word Frame Count

Only applies to `DATA(Nw)` frames (frame type 0011). Set this to the value of
`FSI_TX_FRAME_CTRL.N_WORDS` from your firmware. Valid range: 1–16.

This setting has no effect on fixed-size frame types (PING, ERROR, DATA 1w
through 6w) — those word counts are determined by the frame type field on
the wire.

**Important:** if this value does not match what the firmware transmits, the
analyzer will mis-align on N-word frames, producing incorrect data word
values and likely a CRC failure.

-----

## Frame structure

Each FSI frame has the following fields in order:

| Field | Bits | Present in |
|---|---|---|
|Preamble (4 clocks HIGH) + SOF (1001)|8|All frames|
|Frame Type|4|All frames|
|User Data|8|Data frames only|
|Data Words (N × 16 bits)|N×16|Data frames only|
|CRC|8|Data frames only|
|Frame Tag|4|All frames|
|EOF (0110)|4|All frames|
|Postamble (4 clocks HIGH)|4|All frames|

PING and ERROR frames carry no user data, data words, or CRC — just
Frame Type, Frame Tag, EOF, and Postamble after the preamble.

-----

## Reading the waveform

Each FSI frame produces the following labelled segments on the waveform:

| Label (short) | Label (expanded) | What it shows |
|---|---|---|
|PRE|Preamble|Preamble detected — 4 HIGH bits before frame start|
|SOF|Start of Frame|SOF[0..3] pattern (1001) confirmed — marks the frame start|
|FT|PING / ERROR / DATA(Nw) / …|Frame type decoded from the header|
|UD|UserData: 0xNN|8-bit user data byte (data frames only)|
|D0xNNNN|Data[n]: 0xNNNN|16-bit data word, with word index shown (data frames only)|
|CRC|CRC: 0xNN OK / CRC: 0xNN [BAD]|Received CRC and pass/fail status (data frames only)|
|TAG|Tag: N|4-bit user tag value (0–15)|
|EOF|End of Frame|EOF pattern (0110) validated|
|POST|Postamble|Postamble — four trailing HIGH bits before idle|
|ERR|Framing Error|Bad EOF pattern or sync loss|

Click any bubble to see the full expanded label. The number format (decimal,
hex, binary, ASCII) follows the **Display Radix** setting in the Logic 2
analyzer panel.

### CRC status

The CRC bubble always shows the received byte. If it does not match the
expected value computed from the frame contents, **[BAD]** is appended. A
CRC failure typically means:

- Probing noise or insufficient sample rate
- The N-Word Frame Count setting does not match the firmware
- The captured frame was genuinely corrupted on the wire

-----

## Exporting data

To export decoded frame data as CSV:

1. Right-click the analyzer in the Analyzers panel.
2. Choose **Export**.
3. Select **Export as CSV** and choose a file location.

The CSV contains one row per decoded field with columns:

```
Frame Index, Type, Value, Word Index, CRC OK, Start Sample, End Sample
```

`Word Index` is populated only for `DataWord` rows. `CRC OK` is populated
only for `CRC` rows (OK or FAIL). Start and end sample numbers are in the
capture's sample domain and can be converted to time using the capture sample
rate.

-----

## Troubleshooting

### No frames decoded / analyzer shows nothing

- Verify the channel assignments match your wiring.
- Confirm the capture sample rate is ≥ 4× your FSI clock frequency.

### Every frame shows CRC BAD

- Increase the sample rate. At marginal sample rates, edge detection errors
  cause bit flips that break CRC.
- Verify the N-Word Frame Count matches `FSI_TX_FRAME_CTRL.N_WORDS`.

### Only the first frame is decoded, then nothing

- The preamble sync state machine requires at least 5 consecutive HIGH bits
  followed by SOF (1001) to lock onto a frame. If your firmware sends very
  short inter-frame gaps, the postamble HIGH bits from one frame may merge
  with the preamble of the next, causing the analyzer to miss subsequent
  frames. This is a known limitation.

### Data words look wrong / values seem shifted

- In 2-lane mode, confirm both TXD0 and TXD1 are assigned to the correct
  channels. Swapping them produces incorrect bit interleaving.
- In 1-lane mode, confirm 2-Lane Mode is **off**.
- For N-word frames, confirm the N-Word Frame Count setting matches firmware.

### Logic 2 does not show the analyzer in the list

- Confirm the plugin file is in the folder registered under
  **Edit → Settings → Custom Low Level Analyzers**.
- Logic 2 must be **restarted** after adding a new plugin folder or replacing
  the plugin file.
- On macOS, run the quarantine removal command:
  `xattr -d com.apple.quarantine FSIAnalyzer.dylib`

-----

## Known limitations

- **N-word count must be set manually.** The number of data words in a
  `DATA(Nw)` frame cannot be determined from the wire — it must match the
  firmware configuration exactly.

- **No simulated waveform.** Logic 2's built-in simulation mode is not
  supported. The analyzer must be used with real captured data.

- **Short inter-frame gaps may cause missed frames.** The preamble state
  machine requires a minimum of 5 consecutive HIGH bits before committing to
  a frame start. Very tight back-to-back frame timing may result in one or
  more frames being skipped.

- **Single-ended probing only.** FSI signals probed after an LVDS or
  isolation transceiver are supported. Direct probing of LVDS differential
  pairs is not supported by Saleae hardware.
