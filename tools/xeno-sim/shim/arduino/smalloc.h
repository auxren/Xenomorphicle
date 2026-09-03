#ifndef XENOSIM_SMALLOC_H_
#define XENOSIM_SMALLOC_H_
// cores/teensy4/smalloc.h, absent (not part of software/src -- see
// AudioStream.h's header comment for the general shape of this problem).
// software/src/Audio/AudioBuffer.h only wants extmem_calloc()/extmem_free(),
// which Arduino.h already defines as plain calloc()/free() wrappers -- this
// is just the forwarding header real firmware reaches them through.
#include "Arduino.h"
#endif
