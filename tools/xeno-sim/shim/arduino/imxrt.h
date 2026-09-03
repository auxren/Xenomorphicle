#ifndef XENOSIM_IMXRT_H_
#define XENOSIM_IMXRT_H_
// i.MXRT1062 peripheral register map, absent. Nothing pulled into this
// build's compile set actually uses a symbol from it -- Audio/
// effect_reverb_schroeder_F32.h #includes it but reaches no register
// through it (grep confirms), so an empty stand-in satisfies the include
// with nothing to shim. If a future app drags in a real user of this
// header, that is the same "found the gap" moment the rest of shim/ is
// built from -- not something to pre-empt here.
#endif
