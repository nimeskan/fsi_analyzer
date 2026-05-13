# TI FSI (C2000) Analyzer — User Guide

## What this tool does

The TI FSI (C2000) analyzer decodes **Fast Serial Interface** frames produced
by Texas Instruments C2000 microcontrollers (F280049, F2838x, F28003x, and
compatible devices) directly inside **Saleae Logic 2**.

After adding the analyzer to a capture, every FSI frame is automatically
broken into labelled segments on the waveform: preamble, frame type, tag,
user data, data words, CRC status, and end-of-frame. No manual bit-counting
or spreadsheet work is needed.

-----

## Supported frame types

| Frame type | Description |
|---|---|
|PING |Heartbeat frame — no data words|
|ERROR |Error signalling frame — no data words|
|DATA(1w)|1 × 16-bit data word|
|DATA(2w)|2 × 16-bit data words|
|DATA(4w)|4 × 16-bit data words|
|DATA(6w)|6 × 16-bit data words|
|DATA(Nw)|N × 16-bit data words, where N is configured in the analyzer settings|

-----

## Hardware requirements

### Logic analyzer

| Hardware | Supported |
|---|---|
|Logic Pro 8|Yes — recommended minimum|
|Logic Pro 16|Yes|
|Logic 4|No — insufficient bandwidth|

Capture at **≥ 200 MS/s**. FSI clocks up to 50 MHz with DDR encoding,
giving up to 100 Mbps per data lane. The Logic 4's maximum sample rate is
not sufficient to resolve FSI edges reliably.

### Probing

| FSI signal | Connect to |
|---|---|
|TXCLK / RXCLK|Any Logic channel — assign to TXCLK in settings|
|TXDA / RXD0|Any Logic channel — assign to TXDA/RXD0 in settings|
|TXDB / RXD1|Any Logic channel — assign to TXDB/RXD1 (2-lane only)|

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
3. Go to **Preferences → Custom Low Level Analyzers**.
4. Click **+** and point it at the folder containing the plugin file.
5. **Restart Logic 2**.

The analyzer will appear as **TI FSI (C2000)** when you add a new analyzer
to a capture.

-----

## Adding the analyzer to a capture

1. Take a capture that includes TXCLK and at least TXDA.
2. In the Analyzers panel, click **+**.
3. Search for **TI FSI (C2000)** and select it.
4. Configure the settings (see below) and click **Save**.

The analyzer will run immediately on the captured data. Bubble labels appear
above each channel. On large captures this may take a few seconds.

-----

## Settings reference

### Channel assignments

| Setting | Description |
|---|---|
|**TXCLK / RXCLK**|The FSI clock channel. Required.|
|**TXDA / RXD0**|FSI data lane 0. Required.|
|**TXDB / RXD1**|FSI data lane 1. Only needed when 2-Lane Mode is enabled.|

Assign each setting to the Logic channel you have wired to the corresponding
FSI pin. If TXDB / RXD1 is not assigned and 2-Lane Mode is off, it is ignored.

### Protocol options

#### 2-Lane Mode

Enable this when your firmware configures the FSI peripheral for dual-lane
operation. In 2-lane mode, the peripheral drives two data lines simultaneously:
TXDA carries even-indexed bits and TXDB carries odd-indexed bits, sampled on
the same clock edge. This doubles throughput but requires both lines to be
probed.

Leave this **off** for standard single-lane captures.

#### N-Word Frame Count

Only applies to `DATA(Nw)` frames (frame type 0x6). Set this to the value of
`FSI_TX_FRAME_CTRL.N_WORDS` from your firmware. Valid range: 1–16.

This setting has no effect on fixed-size frame types (PING, ERROR, DATA 1w
through 6w) — those word counts are determined by the frame type field on
the wire.

**Important:** if this value does not match what the firmware transmits, the
analyzer will mis-align on N-word frames, producing incorrect data word
values and likely a CRC failure.

#### SPI-Compatible Mode

Enable this when your firmware sets `FSI_TX_COMPAT_MODE`. In this mode the
FSI peripheral signals frame start using a chip-select assertion on TXDA
instead of the normal flush + SOF preamble. The rest of the frame structure
(header, data words, CRC, EOF) is identical.

Leave this **off** unless you know your firmware uses SPI-compatible mode.

-----

## Reading the waveform

Each FSI frame produces the following labelled segments on the waveform:

| Label (short) | Label (expanded) | What it shows |
|---|---|---|
|PRE / CS|Preamble / SPI-Compat CS|Start of frame detected|
|FT|PING / ERROR / DATA(Nw) / …|Frame type decoded from the header|
|TAG|Tag: N|4-bit user tag value (0–15)|
|UD|UserData: 0xNN|8-bit user data byte|
|D0xNNNN|Data[n]: 0xNNNN|16-bit data word, with word index shown|
|CRC|CRC: 0xNN OK / CRC: 0xNN [BAD]|Received CRC and pass/fail status|
|EOF|End of Frame|EOF pattern (0x9) validated|
|ERR|Framing Error|Bad EOF pattern or sync loss|

Click any bubble to see the full expanded label. The number format (decimal,
hex, binary, ASCII) follows the **Display Radix** setting in the Logic 2
analyzer panel.

### CRC status

The CRC bubble always shows the received byte. If it does not match the
expected value computed from the frame contents, **[BAD]** is appended. A
CRC failure typically means:

- Probing noise or insufficient sample rate
- The CRC polynomial in the analyzer does not match the device revision
  (see Known Limitations)
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
- Confirm the capture sample rate is ≥ 200 MS/s.
- Check that the correct mode is selected: if firmware uses SPI-Compatible
  Mode, enable it in settings; if not, leave it off.

### Every frame shows CRC BAD

- Increase the sample rate. At marginal sample rates, edge detection errors
  cause bit flips that break CRC.
- Verify the N-Word Frame Count matches `FSI_TX_FRAME_CTRL.N_WORDS`.
- Consult your device's TRM — some C2000 revisions use a different CRC
  polynomial. If the polynomial differs, the analyzer's lookup table must be
  regenerated by a developer.

### Only the first frame is decoded, then nothing

- The preamble sync state machine requires at least 6 alternating bits
  followed by 2+ idle low bits before the SOF nibble. If your firmware
  sends very short inter-frame gaps, the analyzer may miss subsequent
  preambles. This is a known limitation.

### Data words look wrong / values seem shifted

- In 2-lane mode, confirm both TXDA and TXDB are assigned to the correct
  channels. Swapping them produces incorrect interleaving.
- In 1-lane mode, confirm 2-Lane Mode is **off**.
- For N-word frames, confirm the N-Word Frame Count setting matches firmware.

### Logic 2 does not show the analyzer in the list

- Confirm the plugin file is in the folder registered under
  **Preferences → Custom Low Level Analyzers**.
- Logic 2 must be **restarted** after adding a new plugin folder or replacing
  the plugin file.
- On macOS, run the quarantine removal command:
  `xattr -d com.apple.quarantine FSIAnalyzer.dylib`

-----

## Known limitations

- **N-word count must be set manually.** The number of data words in a
  `DATA(Nw)` frame cannot be determined from the wire — it must match the
  firmware configuration exactly.

- **CRC polynomial is fixed.** The analyzer uses the standard FSI CRC-8
  polynomial (0x4D). If your device revision uses a different polynomial,
  contact the developer to regenerate the CRC table.

- **No simulated waveform.** Logic 2's built-in simulation mode is not
  supported. The analyzer must be used with real captured data.

- **Short inter-frame gaps may cause missed frames.** The preamble state
  machine requires a minimum flush sequence before committing to a frame
  start. Very tight back-to-back frame timing may result in one or more
  frames being skipped.

- **Single-ended probing only.** FSI signals probed after an LVDS or
  isolation transceiver are supported. Direct probing of LVDS differential
  pairs is not supported by Saleae hardware.

- **SPI-Compatible Mode CS noise.** In SPI-compat mode the analyzer triggers
  on any falling edge of TXDA. Pull-up noise during CS deassertion could
  cause a spurious frame start to be detected.
