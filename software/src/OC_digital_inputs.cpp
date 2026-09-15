#include <Arduino.h>
#include <algorithm>
#include "OC_digital_inputs.h"
#include "OC_gpio.h"
#include "OC_options.h"


#if defined(__IMXRT1062__) // Teensy 4.0 or 4.1
uint32_t OC::DigitalInputs::rising_edges_;
uint32_t OC::DigitalInputs::raised_mask_;
uint32_t OC::DigitalInputs::latched_edges_;
IMXRT_GPIO_t * OC::DigitalInputs::port[DIGITAL_INPUT_LAST];
uint32_t  OC::DigitalInputs::bitmask[DIGITAL_INPUT_LAST];

FLASHMEM
void OC::DigitalInputs::Init() {
  pinMode(TR1, INPUT_PULLUP);
  pinMode(TR2, INPUT_PULLUP);
  pinMode(TR3, INPUT_PULLUP);
  pinMode(TR4, INPUT_PULLUP);
  port[0] = (IMXRT_GPIO_t *)digitalPinToPortReg(TR1);
  port[1] = (IMXRT_GPIO_t *)digitalPinToPortReg(TR2);
  port[2] = (IMXRT_GPIO_t *)digitalPinToPortReg(TR3);
  port[3] = (IMXRT_GPIO_t *)digitalPinToPortReg(TR4);
  bitmask[0] = digitalPinToBitMask(TR1);
  bitmask[1] = digitalPinToBitMask(TR2);
  bitmask[2] = digitalPinToBitMask(TR3);
  bitmask[3] = digitalPinToBitMask(TR4);
  for (unsigned int i=0; i < 4; i++) {
    unsigned int bitnum = __builtin_ctz(bitmask[i]);
#ifdef ARDUINO_TEENSY41
    if (bitnum < 16) {
      // rising edge detect, bits 0-15
      port[i]->ICR1 = (port[i]->ICR1 & ~(0x03 << (bitnum * 2))) | (0x02 << (bitnum * 2));
    } else {
      // rising edge detect, bits 16-31
      port[i]->ICR2 = (port[i]->ICR2 & ~(0x03 << ((bitnum - 16) * 2))) | (0x02 << ((bitnum-16) * 2));
    }
#else
    if (bitnum < 16) {
      port[i]->ICR1 |= (0x03 << (bitnum * 2)); // falling edge detect, bits 0-15
    } else {
      port[i]->ICR2 |= (0x03 << ((bitnum - 16) * 2)); // falling edge detect, bits 16-31
    }
#endif
    port[i]->ISR = bitmask[i]; // clear any prior detected edge
  }
}

void OC::DigitalInputs::Scan() {
  uint32_t mask[4];
  noInterrupts();
  mask[0] = port[0]->ISR & bitmask[0];
  port[0]->ISR = mask[0];
  mask[1] = port[1]->ISR & bitmask[1];
  port[1]->ISR = mask[1];
  mask[2] = port[2]->ISR & bitmask[2];
  port[2]->ISR = mask[2];
  mask[3] = port[3]->ISR & bitmask[3];
  port[3]->ISR = mask[3];
  interrupts();
  uint32_t new_clocked_mask = 0;
  if (mask[0]) new_clocked_mask |= 0x01;
  if (mask[1]) new_clocked_mask |= 0x02;
  if (mask[2]) new_clocked_mask |= 0x04;
  if (mask[3]) new_clocked_mask |= 0x08;
  rising_edges_ = new_clocked_mask;
  latched_edges_ |= new_clocked_mask;

  uint32_t raised_mask = 0;
  if (read_immediate<DIGITAL_INPUT_1>()) raised_mask |= DIGITAL_INPUT_1_MASK;
  if (read_immediate<DIGITAL_INPUT_2>()) raised_mask |= DIGITAL_INPUT_2_MASK;
  if (read_immediate<DIGITAL_INPUT_3>()) raised_mask |= DIGITAL_INPUT_3_MASK;
  if (read_immediate<DIGITAL_INPUT_4>()) raised_mask |= DIGITAL_INPUT_4_MASK;
  raised_mask_ = raised_mask;
}

#endif // Teensy 4.0 or 4.1
