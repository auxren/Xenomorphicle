#ifndef XENOSIM_SIM_CAPTURE_H_
#define XENOSIM_SIM_CAPTURE_H_

#include <stdint.h>

#include <string>
#include <vector>

// Loads a console `PresetBus: card image dump` hex file -- the format the
// firmware's own DumpCard() prints and the format the bench captures on disk
// are in:
//
//   PresetBus: card image dump, 63120 bytes (last master transfer)
//   0000: BF 80 00 00 3F 8A 05 03 00 00 00 00 00 00 00 00<CR><LF>
//
// NOTE THE CRLF. These files come off a serial console, every data line ends
// "\r\n", and a parser that does not strip the trailing '\r' silently matches
// nothing and returns an empty bank. That has bitten this project before.
//
// Returns true and fills `out` on success; on failure returns false and puts a
// reason in `err`. Bytes land at their stated offsets, so a dump with holes
// comes back zero-filled rather than shifted.
bool SimLoadHexDump(const std::string &path, std::vector<uint8_t> &out,
                    std::string &err);

#endif  // XENOSIM_SIM_CAPTURE_H_
