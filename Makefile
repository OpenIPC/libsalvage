CFLAGS  ?= -O2 -g -std=c11 -Wall -Wextra -Wpedantic
CPPFLAGS += -Iinclude

SRC := src/flexfec.c

all: salvage-pcap test-flexfec

salvage-pcap: $(SRC) tools/salvage-pcap.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $^

test-flexfec: $(SRC) tests/flexfec.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $^

# Unit tests, then a replay of a real camera capture at four loss rates. The
# replay compares every rebuilt packet against the original in the capture, so
# a plausible-but-wrong reconstruction fails here rather than reaching a
# decoder, where the damage would only show up as a picture artefact.
check: all
	./test-flexfec
	@echo
	@for d in 50 20 10 5; do \
	    ./salvage-pcap --drop $$d tests/camera-fec.pcap | \
	        grep -E 'dropped by us|byte-exact|WRONG|recovery rate' || exit 1; \
	    echo; \
	done

asan: CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
asan: clean check

clean:
	rm -f salvage-pcap test-flexfec

.PHONY: all check clean asan
