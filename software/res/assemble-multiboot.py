Import("env")

import subprocess
from os.path import join

def get_git_rev():
    git_rev = subprocess.check_output(["git", "rev-parse", "--short", "HEAD"]).decode().strip()
    git_status = subprocess.check_output(["git", "status", "-s", "--untracked-files=no"]).decode()
    suffix = "dirty" if git_status else ""

    return git_rev + suffix

def get_version():
    with open("src/OC_version.h") as file:
        last_line = file.readlines()[-1].strip().replace('"', '')

    return last_line 

git_rev = get_git_rev()
env.Append(BUILD_FLAGS=[ f'-DOC_BUILD_TAG=\\"{git_rev}\\"' ])

def after_build(source, target, env):
    git_rev = get_git_rev()
    version = get_version()
    build_flags = env.ParseFlags(env['BUILD_FLAGS'])
    defines = build_flags.get("CPPDEFINES")
    for item in defines:
        if item[0] == 'OC_VERSION_EXTRA':
            version += item[1].strip('"')
    env.Replace(PROGNAME=f"xenomorphicle-{version}-{git_rev}")

    app_A = env.subst(".pio/build/T41/firmware.hex")
    app_B = env.subst(".pio/build/T41_audio/firmware.hex")
    app_X = env.subst(".pio/build/T41_MTP/firmware.hex")
    # app_Y = env.subst("")

    out = env.subst("${PROGNAME}.hex")

    concat_intel_hex([app_A, app_B, app_X], out)


def concat_intel_hex(sources, out):
    """Concatenate Intel HEX files into one multiboot image.

    This used to shell out to tool-sreccat's srec_cat. PlatformIO ships that
    only as darwin_x86_64, and macOS 27 dropped Rosetta, so it stopped being
    runnable at all ("Bad CPU type in executable") -- the firmware linked fine
    and only the stitching failed. The three slot images occupy disjoint,
    already-ordered address ranges (0x60000000 / 0x60100000 / 0x60200000), so
    the whole job is: every record from every file, minus their end-of-file
    records, plus one end-of-file at the end. Doing that here costs nothing and
    removes an architecture-specific tool from the build.
    """
    EOF = ":00000001FF"
    # Address order, not argument order. The callers pass slot 1 first, and
    # srec_cat used to sort records internally; concatenating as given produced
    # an image whose addresses ran backwards at the first seam. Each file is
    # internally ascending and the three ranges are disjoint, so sorting the
    # FILES by their lowest address is enough to make the whole image ascending.
    sources = sorted(sources, key=_base_address)
    with open(out, "w") as dst:
        for src in sources:
            with open(src) as f:
                for line in f:
                    line = line.strip()
                    if not line.startswith(":"):
                        continue
                    # record type is bytes 7..8; 01 is end-of-file
                    if line[7:9].upper() == "01":
                        continue
                    dst.write(line + "\n")
        dst.write(EOF + "\n")

def _base_address(path):
    """Lowest absolute address of any data record in an Intel HEX file."""
    ext = 0
    lowest = None
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line.startswith(":"):
                continue
            rectype = line[7:9].upper()
            if rectype == "04":          # extended linear address
                ext = int(line[9:13], 16) << 16
            elif rectype == "00":        # data
                addr = ext + int(line[3:7], 16)
                if lowest is None or addr < lowest:
                    lowest = addr
    return lowest if lowest is not None else 0


env.AddPostAction("buildprog", after_build)
