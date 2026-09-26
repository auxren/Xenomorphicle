// USB string descriptors. Must live in a .c file, not a .cpp or the sketch:
// the core declares these as weak aliases (usb_desc.c:2871-2876) and the
// override has to have C linkage to replace them.

#include "usb_names.h"

// Edit these lines to create your own name.  The length must
// match the number of characters in your custom name.

// What the host calls the device: the module.
#define MIDI_NAME {'X','e','n','o','m','o','r','p','h','e','r'}
#define MIDI_NAME_LEN 11

// Who made it: this firmware. Without this the host shows "Teensyduino",
// which is the toolchain rather than the instrument -- visible in every DAW's
// device list and in macOS's audio/MIDI setup.
#define MANUF_NAME {'X','e','n','o','m','o','r','p','h','i','c','l','e'}
#define MANUF_NAME_LEN 13

// Do not change this part.  This exact format is required by USB.

struct usb_string_descriptor_struct usb_string_product_name = {
        2 + MIDI_NAME_LEN * 2,
        3,
        MIDI_NAME
};

struct usb_string_descriptor_struct usb_string_manufacturer_name = {
        2 + MANUF_NAME_LEN * 2,
        3,
        MANUF_NAME
};
