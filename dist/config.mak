#
# sinew config.mak template (original in dist/config.mak)
#
# SINEW libc build configuration baseline. This file is documentation:
# ./configure generates the working config.mak from its own defaults
# plus the environment it is given, so these settings reach the
# compiler only when passed to configure (CFLAGS=...) or appended to
# the generated config.mak the way the Saphira recipe does. Keep this
# template and the recipe in agreement.
#

# Target CPU architecture. SINEW builds x86_64 only.
ARCH = x86_64

# SINEW baseline: x86-64 microarchitecture level 3.
CFLAGS += -march=x86-64-v3

# Installation layout. SINEW installs natively to /usr (dynamic
# loader at /lib/ld-musl-x86_64.so.1 via --syslibdir=/lib); there is
# no standalone /usr/local/musl prefix and no musl-gcc wrapper path.
prefix = /usr
syslibdir = /lib

# Static-PIE policy (Saphira): the static archive must be built from
# the PIC object set so static-PIE links. The recipe appends
#	AOBJS = $(LOBJS)
# to the generated config.mak; any build outside the recipe must do
# the same, and the piecheck (no absolute R_X86_64_32 relocations in
# libc.a) must stay green.

# Uncomment to fix broken distro-patched toolchains where hash-style=gnu(only)
#LDFLAGS += -Wl,--hash-style,both

# Uncomment to fix broken distro-patched toolchains where stack-protector=on
#CFLAGS += -fno-stack-protector

# Uncomment for smaller code size.
#CFLAGS += -fomit-frame-pointer -mno-accumulate-outgoing-args

# Compiler warnings (as errors). This is the SINEW baseline, not a
# suggestion: -Wall -Wextra -Werror plus -Wpointer-arith and
# -Wcast-align. The historical musl -Wno-* suppression list is
# deliberately NOT carried forward — warnings are exposed and fixed
# at their source, never hidden. Upstream musl code that warns under
# these flags is fixed minimally or reported with cause; no blanket
# suppressions are added to satisfy -Werror.
CFLAGS += -Wall -Wextra -Werror -Wpointer-arith -Wcast-align

# Uncomment if you want to disable building the shared library.
#SHARED_LIBS =
