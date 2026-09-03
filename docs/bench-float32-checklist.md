# Bench checklist — float32/24-bit branch validation (task #7)

Branch: `float32-24bit`. Build/flash `T41_audio` (slot 0). Compare against stock v2.0.1 where noted.

## 1. Boot + basics
- [ ] Boots, UI responsive, no boot loop; audio applets load/swap without hang
- [ ] Codec passthrough: analog in → Input applet → out is clean at unity

## 2. USB enumeration (each host OS available)
- [ ] Device appears as class-compliant audio, **24-bit / 48 kHz / 4-in / 4-out**
  - macOS: Audio MIDI Setup → Format shows 24-bit int, 48kHz, 4ch each way
  - Windows: Settings → Sound; UAC2 class driver (no vendor driver)
  - Linux: `cat /proc/asound/*/stream0` → shows S24_3LE, rates, channels
- [ ] USB MIDI still enumerates alongside audio (composite device intact)
- [ ] Playback ch1/2 reaches applet bus (Input applet, USB source); ch3/4 monitors to codec out
- [ ] Capture ch1/2 = engine out; ch3/4 = codec inputs (record raw analog)
- [ ] 30-min stream both directions: no glitches/dropouts (async feedback stable)

## 3. Quality A/B (vs v2.0.1 16-bit build)
- [ ] Noise floor of capture ch3/4 with inputs terminated — expect ≤ old build, LSBs active beyond bit 16
- [ ] Delay applet: long feedback tail — listen for absence of int16 grit as tail decays
- [ ] Freeverb/Samverb tails: same character, cleaner decay into silence
- [ ] Ladder self-oscillation clean; FilterFolder transfer sounds unchanged
- [ ] Presets saved under v2.0.1 load identically (formats are byte-identical by design)

## 4. Performance
- [ ] CPU headroom with a full applet chain (use built-in CPU/peak displays); compare vs v2.0.1
- [ ] Round-trip latency, USB out→in loopback (e.g. RTL Utility on host, or scope analog out vs DAW click)
- [ ] Flash `T41_audio_blk64` (block=64): repeat latency + CPU; keep if stable, note both numbers

## Fail-fast notes
- No enumeration at 24-bit → check `AUDIO_SUBSLOT_SIZE=3` reached usb_desc (bInterface bit depth), try another cable/port first
- Distortion/level wrong on one path only → suspect a conversion scale (2^15 vs 2^23) at that boundary; sample_convert.h is host-tested, edge adapters are not
- Periodic clicks at block rate → update-order/latency issue in AudioIO lazy construction; compare block 128 vs 64 behavior
