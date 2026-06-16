# Two builds:
#   make           -> native Mac/PC binary (uses system libsqlite3) for testing
#   make device    -> fully-static ARMv7 musl binary for the Miyoo Mini Plus
#                     (SQLite compiled in; zero runtime dependencies)

CC      ?= cc
CFLAGS  ?= -std=c99 -Wall -Wextra -O2
SRC      = src/main.c src/tracker.c src/menu.c
HDR      = src/tracker.h

# --- native (Mac) build: link the system sqlite3 -----------------------------
LDLIBS   = -lsqlite3
ifneq ($(wildcard /opt/homebrew/opt/sqlite),)
  CFLAGS  += -I/opt/homebrew/opt/sqlite/include
  LDFLAGS += -L/opt/homebrew/opt/sqlite/lib
endif

tracker: $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o tracker $(SRC) $(LDFLAGS) $(LDLIBS)

# --- device (ARMv7 musl, static) build ---------------------------------------
# Cortex-A7 = armv7-a hardfloat + NEON. SQLite amalgamation compiled directly in.
ARMCC    ?= armv7-unknown-linux-musleabihf-gcc
ARMFLAGS  = -std=c99 -O2 -static \
            -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard \
            -Ithird_party/sqlite \
            -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_DQS=0
SQLITESRC = third_party/sqlite/sqlite3.c

device: $(SRC) $(HDR) $(SQLITESRC)
	$(ARMCC) $(ARMFLAGS) -o tracker.arm $(SRC) $(SQLITESRC) -lm
	@echo "built tracker.arm:" && file tracker.arm 2>/dev/null || true

# --- framebuffer/input smoke test (ARMv7 musl, static, no sqlite) ------------
fbtest: src/fbtest.c
	$(ARMCC) -std=c99 -O2 -static -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard \
	         -o fbtest.arm src/fbtest.c
	@file fbtest.arm 2>/dev/null || true

# --- GUI (ARMv7 musl, static): gfx + input + box art + tracker core + sqlite -
GUISRC = src/gui.c src/gfx.c src/input.c src/tracker.c $(SQLITESRC)
gui: $(GUISRC) src/gfx.h src/input.h src/tracker.h src/font.h third_party/stb_image.h
	$(ARMCC) -O2 -static -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard \
	         -Ithird_party -Ithird_party/sqlite -Isrc \
	         -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_DQS=0 \
	         -o minitracker.arm $(GUISRC) -lm
	@file minitracker.arm 2>/dev/null || true

# --- guided button-mapping tool (ARMv7 musl, static; gfx only) ---------------
keytest: src/keytest.c src/gfx.c src/gfx.h src/font.h
	$(ARMCC) -O2 -static -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard -Isrc \
	         -o keytest.arm src/keytest.c src/gfx.c
	@file keytest.arm 2>/dev/null || true

clean:
	rm -f tracker tracker.arm fbtest.arm minitracker.arm keytest.arm

.PHONY: clean device fbtest gui keytest
