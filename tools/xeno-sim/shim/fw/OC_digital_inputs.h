#ifndef OC_DIGITAL_INPUTS_H_
#define OC_DIGITAL_INPUTS_H_
// Host stand-in for software/src/OC_digital_inputs.h.
//
// PresetBusUI::Task() samples TR1-TR4 every pass for the 225e-style last/next
// pulse jacks. The simulator models no trigger inputs at all, so these read low
// forever: the assignment cursor and its jack dots are live in the overlay, but
// nothing will ever pulse them here. Said out loud rather than faked, because a
// fake pulse would recall bus-wide and the whole point of the overlay is that
// that is a real, bus-wide action.

namespace OC {

enum DigitalInput {
  DIGITAL_INPUT_1,
  DIGITAL_INPUT_2,
  DIGITAL_INPUT_3,
  DIGITAL_INPUT_4,
  DIGITAL_INPUT_LAST
};

namespace DigitalInputs {
bool read_immediate(DigitalInput input);
}  // namespace DigitalInputs

}  // namespace OC

#endif  // OC_DIGITAL_INPUTS_H_
