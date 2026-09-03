Import("env")

# Force-include src/OC_flashmem.h into every Teensy 4 translation unit (core,
# libraries and ours alike). See that header for why: GCC's LTO drops the
# section attribute of in-class FLASHMEM functions, so without this the
# applets' View()/OnEncoderMove() land in ITCM and eat DTCM stack.
# A pre-script rather than a build_flags entry because PlatformIO's flag
# parser silently discards "-include".
if env.BoardConfig().get("build.mcu", "") == "imxrt1062":
    env.Append(CCFLAGS=["-include", "src/OC_flashmem.h"])
