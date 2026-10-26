# RAM2 budget: what takes the heap, and when

The Xenomorpher is a Teensy 4.1. Its RAM2 (OCRAM, 512 KB) holds three things:
static `DMAMEM` objects, the malloc heap, and nothing else. The linker reports
what is left for the heap ("free for malloc/new"); this page lists what is then
taken out of it, per app and at boot, so a large request (the 229,376-byte
firmware card, `kFwCardV2Size`) can be budgeted against what is actually there.

Console `t` prints `heap free` (the untouched top only), `holes`, `hole_free` and
`largest_run` (see [bench-console.md](bench-console.md)). The card needs one
contiguous run of 229,376 bytes, so `largest_run` is the number to watch, not
`heap free`.

All sizes below are computed from source constants or read from the linked
image (`arm-none-eabi-nm -S`). Nothing in the "runtime" tables was measured on a
running module; the boot figure is the only measured one (about 250 KB free
after a freshly started build, against 337,216 bytes at link time).

## Link time (T41_console, 2026-10)

| Environment | RAM2 variables | free for malloc/new |
| --- | --- | --- |
| T41_console | 187,072 | 337,216 |
| T41_audio | 176,352 | 347,936 |
| T41_audio_dbg | 187,936 | 336,352 |

Largest static RAM2 objects in T41_console (bytes): AudioIO `data` 65,520 (the
int16 audio block pool, `AUDIO_MEMORY` = 252), PresetEngine `stage_arena`
24,576, app container 16,464, `sec_buf` 16,384, `stage_tmp` 12,288, USB TX/RX
buffers about 17,400, calibration flow stores about 7,600, audio-graph edge
table 4,096. These are fixed and never reach the heap.

## At boot (heap)

| Allocation | Bytes | Where |
| --- | --- | --- |
| F32 audio block pool, `new audio_block_f32_t[128]` x 532 | 68,096 | `AudioIO.cpp`, `AudioMemory_F32(F32_AUDIO_MEMORY)` |
| AudioIO converters, mixers, I2S output, cables | small, a few KB | `AudioIO.cpp:137-178` |
| Quadrants default applets (Registry factory, `calloc`) | per applet, see below | `AppletRegistry.h` |
| Armed firmware card image | 229,376 (v2), 65,536 (v1) | `PresetBus.cpp`, `malloc` |

The pool plus small objects account for most of the roughly 87 KB that a fresh
boot takes before the card; the remainder is applet instances, LittleFS and USB
bookkeeping (not itemised).

## Per app, once opened

| App | Bytes of RAM2 heap | Notes |
| --- | --- | --- |
| Scope | about 70,000 | `malloc(sizeof(Arena))` on open, freed on close (`ScopeApp.h`); the model for lazy apps |
| Sampler | 0 idle; about 28,700 per playing voice, up to about 229,000 for 8 | SD read-ahead, `BUFFER_COUNT_SD` 7 x `BUFFER_SIZE_SD` 2048 x int16, per file in use; freed when the file closes |
| Tweighty, PSRAM fitted | 0 (buffers 2 x 2 MB in PSRAM) | `ExtAudioBuffer`, `extmem_calloc` |
| Tweighty, no PSRAM | 131,072 | the same call falls back to `calloc` (see below); 2 x 16,384 floats |
| Tweighty crossfade table | 0 (was 16,384) | now a shared constant in flash |
| Delay, PSRAM fitted | 0 heap | 2 x 2 MB PSRAM |
| Delay, no PSRAM | 131,072 | 2 x (524,288 / 32) floats, same fallback |
| Delay crossfade table | 0 (was 32,768 stereo) | shared constant in flash |
| Reverb (Freeverb) | 50,348 | Factory block, held until the next reboot |
| Samverb / Bungverb | 77,792 | Factory block, held until the next reboot |
| Abyss | 230,028 | `malloc`, PSRAM fallback; held until the next reboot |

### The core's external-memory fallback

`extmem_malloc` / `extmem_calloc` in the pinned core (`cores/teensy4/extmem.c`)
try the PSRAM pool and, when that returns null, return `malloc` / `calloc`
from RAM2. With no PSRAM chip fitted the pool is empty, so every "PSRAM" buffer
above silently becomes a RAM2 allocation. A module without the chip therefore
pays for Tweighty and Delay out of the same heap the card needs. Earlier notes
in `docs/ui-redesign` describe the no-PSRAM Delay as silent; that is only true
when the heap cannot supply the reduced buffer.

## What does not give memory back

Two mechanisms hold RAM2 until the next reboot after a single visit:

* `Factory::release()` (`OC_core.h`) only clears a "used" bit; the pooled block
  stays allocated and is handed to the next caller of the same type. Releasing an
  effect on eviction therefore returns nothing to the heap.
* Freeing the block instead would destroy an `AudioStream` that is still on the
  library's global update list (the core's `AudioStream` has no destructor that
  unlinks it), so a later audio interrupt walks freed memory. This cannot be
  checked on the host.

## Changes made for this budget

* Crossfade tables (Tweighty and the F32 delay): compile-time constant in flash,
  one shared copy, no allocation. Saves 16 KB of heap per Tweighty and 16 KB per
  delay channel. Linked image: flash grows by 16,392 bytes, RAM1 variables and
  RAM2 are unchanged (the table is kept out of DTCM on purpose, see
  `Audio/XfadeTable.h`).
* Sampler read-ahead buffers can be asked for in PSRAM with `-DXENO_SAMPLER_PSRAM`
  and only when `external_psram_size` is non-zero. Off in every shipped
  environment; without the flag, or without the chip, the call is the constant
  `false` it always was.

## Not done, and the bench test each needs

* Effects releasing their buffers on eviction. Needs the Factory to free blocks,
  which needs a proof that an audio interrupt cannot reach a destroyed stream.
  Bench: open Reverb, Samverb and Abyss in turn on a module with `t` before and
  after each; evict; confirm `heap free` / `largest_run` rise; leave audio
  running with a signal patched for several minutes and watch for a fault.
* Turning `XENO_SAMPLER_PSRAM` on. Needs a board with the chip: play 8 voices
  from SD at once with it off and on, compare audio CPU in `t` and listen for
  dropouts, and confirm `largest_run` rises by the buffer size.
