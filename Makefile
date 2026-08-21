CFLAGS  ?= -O2 -g -std=c11 -Wall -Wextra -Wpedantic
CPPFLAGS += -Iinclude

SRC := src/flexfec.c src/depay.c src/bits.c src/retarget.c \
       src/salvage.c

# --- optional hardware decode backends ---------------------------------------
# Compiled in when their libraries are present, so the same tree builds a null
# player on a workstation, a GStreamer player on Intel, and an MPP player on
# Rockchip. Override *_CFLAGS / *_LIBS to point at a non-pkg-config install.
DEC_SRC     := src/decoder.c
DEC_CPPFLAGS :=
DEC_LIBS    :=

GST_PKGS := gstreamer-1.0 gstreamer-app-1.0
ifeq ($(shell pkg-config --exists $(GST_PKGS) 2>/dev/null && echo 1),1)
  DEC_SRC      += src/decoder_gst.c
  DEC_CPPFLAGS += -DSALVAGE_WITH_GST $(shell pkg-config --cflags $(GST_PKGS))
  DEC_LIBS     += $(shell pkg-config --libs $(GST_PKGS))
  BACKENDS     += gst
endif

MPP_CFLAGS ?= $(shell pkg-config --cflags rockchip_mpp 2>/dev/null)
MPP_LIBS   ?= $(shell pkg-config --libs rockchip_mpp 2>/dev/null)
ifneq ($(MPP_LIBS),)
  DEC_SRC      += src/decoder_mpp.c
  DEC_CPPFLAGS += -DSALVAGE_WITH_MPP $(MPP_CFLAGS)
  DEC_LIBS     += $(MPP_LIBS)
  BACKENDS     += mpp
endif

all: salvage-pcap salvage-play test-flexfec test-depay test-salvage
	@echo "decoder backends built in: null $(BACKENDS)"

salvage-pcap: $(SRC) tools/salvage-pcap.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $^

salvage-play: $(SRC) $(DEC_SRC) tools/salvage-play.c
	$(CC) $(CFLAGS) $(CPPFLAGS) $(DEC_CPPFLAGS) -o $@ $^ $(DEC_LIBS)

test-flexfec: $(SRC) tests/flexfec.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $^

test-depay: $(SRC) tests/depay.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $^

test-salvage: $(SRC) tests/salvage.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $^

# Unit tests, then a replay of a real camera capture at four loss rates. The
# replay compares every rebuilt packet against the original in the capture, so
# a plausible-but-wrong reconstruction fails here rather than reaching a
# decoder, where the damage would only show up as a picture artefact.
check: salvage-pcap test-flexfec test-depay test-salvage
	./test-flexfec
	@echo
	./test-depay
	@echo
	./test-salvage
	@echo
	./salvage-pcap --verify-retarget tests/camera-fec.pcap | \
	    grep -E 'identity retarget' || exit 1
	@echo
	@for d in 50 20 10 5; do \
	    ./salvage-pcap --drop $$d tests/camera-fec.pcap | \
	        grep -E 'dropped by us|byte-exact|WRONG|recovery rate' || exit 1; \
	    echo; \
	done

asan: CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
asan: clean check

clean:
	rm -f salvage-pcap salvage-play test-flexfec test-depay test-salvage

.PHONY: all check clean asan
