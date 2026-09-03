#ifndef XENOSIM_SELFTEST_H_
#define XENOSIM_SELFTEST_H_
// --test-phzconfig: exercises PhzConfig's codec (serialize/deserialize,
// save_config/load_config, save_filtered) against the RAM volume, without
// booting the firmware. Returns the process exit status.
int SimPhzConfigSelfTest();
#endif
