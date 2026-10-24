# Xenomorpher (2X0 v2.0) schematics

Northern Light Modular's Teensy 4.1 board pair, as Eagle 9.7 XML:

| file | board |
|---|---|
| `2X0.Digital.2.0.sch` | Teensy 4.1, DAC8568 (CV out), MCP33131D + 74HC4051 (CV in), PCM1808 ADC + PCM1754 DAC (audio), MIDI (H11L1 / 74LV1T125), I2C level shifter, supplies, HW_ID divider |
| `2X0.analog.2.0.sch` | jacks, CV in/out op-amp stages, audio in/out stages, LM339 trigger comparators, OLED, encoders, buttons, USB-C |
| `*.netlist.txt` | flat netlists extracted from the XML (power/ground symbols omitted); regenerate with `tools/eagle_netlist.py <file.sch>` |

The boards join pin-for-pin: digital JP1 = analog JP2 (CV out, audio), digital
JP2 = analog JP3 (CV in, bias rails), JP3/JP4 = JP4/JP5 (panel controls, OLED).

## Teensy pin map (checked against `software/src/OC_gpio.cpp` `Pinout_Detect()`, 2026-10-24: all match)

| pin | net | pin | net |
|---|---|---|---|
| 0 | TR1 | 22 | TR4 |
| 1 | TR2 | 23 | TR3 |
| 2 | I2S2 data out (PCM1754) | 24 | encL push |
| 3 | I2S2 LRCLK | 25 | encR push |
| 4 | I2S2 BCLK | 26 / 27 | OLED MOSI / SCK (SPI1) |
| 5 | I2S2 data in (PCM1808) | 28 | X |
| 6 / 7 / 32 | CV-in mux A1 / A0 / A2 | 29 | A |
| 8 / 9 / 12 | MCP33131D SCK / CNVST / SDO (FlexIO) | 30 / 31 | encL B / A |
| 10 / 11 / 13 | DAC8568 SYNC / DIN / SCLK (SPI) | 33 | I2S2 MCLK |
| 14 | B | 34 / 35 | MIDI in / out (Serial8) |
| 15 | Y | 36 / 37 | encR B / A |
| 16 / 17 | DEBUG header | 38 / 39 / 40 | OLED RST / DC / CS |
| 18 / 19 | I2C SDA / SCL (200e bus, via FET shifter) | 41 (A17) | HW_ID |
| 20 | Z ("ZAP") | | |

## Signal chains worth knowing

- **HW_ID** = 3.3 V × 4k7 / (150k ∥ 33k + 4k7) ≈ **0.49 V** (test pad "HWID_0.5V"):
  `NorthernLightModular` and `Large_OLED` true, `DAC_20Vpp`/`CalSynthXL`/`NLMSerge` false.
- **CV in (8)**: jack → 100 Ω → buffer (TL074) → gain trimmer → inverting stage
  (MCP604, −44k9/100k, offset from −5 V) → mux → MCP33131D (5.0 V ref, AIN− = ground).
  Roughly 0 V → 4.5 V, +10 V → 0 V at the ADC: **inverted, 0–10 V range**, DC-coupled.
  Mux order X0..X7 = CV5..8, CV1..4, which is exactly how `OC_ADC.cpp` `Scan_DMA()`
  assigns frame slots 0..7. FlexIO scans at 300 kHz / 8 = 37.5 kHz per channel before
  the firmware averages frames down.
- **CV out (8)**: DAC8568 (2.5 V ext. ref, 0–5 V) A..H → trimmer → inverting ×4.99 with
  BIAS1 offset → inverting ×1 → 220 Ω → jack. Two inversions, i.e. **non-inverting**,
  which is why NLM sets `DAC_is_inverted` (skip the reference design's `MAX - value` flip).
  Channel order is straight A..H.
- **Audio in (2)**: jack → **1 µF / 100 kΩ AC coupling (≈1.6 Hz high-pass)** → MCP6022
  inverting ×0.162 around the PCM1808 VREF → PCM1808 (I2S, slave, FMT = MD0 = MD1 = 0,
  MCLK from the Teensy). PCM1808 full scale is 0.6 × 5 V = 3.0 Vpp, so **digital full
  scale ≈ 18.5 Vpp (±9.3 V) at the jack**. DC and sub-2 Hz signals don't survive.
- **Audio out (2)**: PCM1754 (FMT = 0: I2S, de-emphasis and mute tied off) → LM833 stage → jack.
- **Triggers (4)**: jack → 100k/100k divider → LM339 (+ input, 1 M hysteresis) against
  TRESH = 1.5 V, so the gate threshold is ≈ 3 V at the jack. Open-collector output pulled up
  to 3.3 V, **active high**, matching `OC_digital_inputs.h` (`activated = HIGH` when
  `ADC33131D_Uses_FlexIO`).
- **200e bus I2C**: Teensy 18/19 with 4k7 pull-ups to 3.3 V → FDV303N (gate at 3.3 V)
  bidirectional FET shifter → `I2C_SDA_IN`/`I2C_SCL_IN` wire pads and a Qwiic connector
  (pin 2 = 3.3 V through a polyfuse). There are no pull-ups on the bus side, so its high level
  is whatever the 200e bus pulls to, and the Teensy pins only ever see 3.3 V.
- **MIDI**: H11L1 opto in → pin 34, 74LV1T125 buffer out from pin 35, TRS (PJ-392A) jacks.

## Reference chain

Every precision voltage comes from the DAC8568's internal 2.5 V reference (enabled by
`0x08000001` in `OC_DAC.cpp`, net `2.5VREF`). Digital-board U4 (OPA2277) makes
`5.0REF` = 2 × 2.5VREF and passes it through R3 / L4 / C45 to the MCP33131D VREF
(`5.0REFUH`). It also makes −5.0 V = −2 × 2.5VREF, the CV-input offset rail. Outputs,
ADC full scale and the input offset are therefore ratiometric: reference drift cancels
in an output→input loopback, but not against an external meter. The CV-out 0 V offset
is one shared trimmer for all 8 outputs (TR1, net `BIAS1`); gain is trimmed per channel.

Each CV output also drives a blue LED: 100k → LED → `LED-BRANCH` → D3–D5 to DGND,
plus R79 2k to −12 V. That draws a standing ~5 mA returning in digital ground.

## Bench-confirmed (2026-10-24, Cal Routine session)

- 0 V input reads raw ≈ 3635 on every input (schematic predicts ≈ 3685). `CVRANGE`
  gives −1.35 V … +10.65 V on input 1. So there IS headroom below 0 V, and
  `docs/CV-Meter.md`'s "0 V is the rail" is wrong. The Cal Routine work is correcting it.
- Inputs top out near 10.6–10.8 V, about 2.75 mV per count. That is the MCP604's ground
  rail (Vadc = 4.50 V − 0.449·k·Vin, with the gain trimmer at k ≈ 0.945), not the ADC.
- Patching an output into a Xeno input lowers it by a constant ~1.2 mV at 1.2 V and at
  9.6 V. The input load is 1 MΩ (proportional), and the output loop senses after its 220 Ω,
  so a constant shift points to a ground/return path, not the input load.

## Schematic anomaly (unchecked)

The netlist puts IC14/IC15 (the CV-in TL074 followers) symbol pin V+ (pad 4) on −12VA
and V− (pad 11) on +12VA. That would reverse-power a real TL074. The inputs work, so
the PCB presumably differs from the drawing; a DMM on IC14 pin 4 (expect +12 V) would
settle it.
