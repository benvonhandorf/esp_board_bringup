# Host-side tests for the audio signal processing.
#
# These run on the build machine, not on a board, and they exist because they
# catch a class of bug hardware cannot. The spectrum analysis in capture.c once
# read a tone falling between two probe frequencies 89 dB low; on a board that
# looks exactly like a dead microphone, and the evidence points at the part
# rather than at the arithmetic. A gcc test found it in seconds.
#
#   make        build and run both
#   make clean

CFLAGS := -O2 -Wall -Wextra -std=gnu11
LDLIBS := -lm

TESTS := tonetest capturetest

.PHONY: all clean
all: $(TESTS)
	@echo
	@for t in $(TESTS); do echo "== $$t"; ./$$t || exit 1; echo; done

tonetest: tonetest.c
capturetest: capturetest.c

clean:
	rm -f $(TESTS)
