# KidBright µAI — host tool (uai) + board binaries (cross-compiled)
#
#   make            build bin/uai (host) and bin/hello (board)
#   make uai        build just the host serial tool
#   make hello      cross-compile the board test program
#   make deploy     build hello and push+run it on the board via uai
#   make monitor    open a read-only serial console
#   make clean

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra

# Cross toolchain for the V831 (Cortex-A7, hard-float, NEON/VFPv4, musl).
# Needs /opt/homebrew/bin on PATH: brew install messense/macos-cross-toolchains/arm-unknown-linux-musleabihf
CROSS       ?= arm-unknown-linux-musleabihf-gcc
CROSSFLAGS  ?= -O2 -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard

UAI := ./bin/uai

.PHONY: all uai hello fbtest clean deploy deploy-fb monitor term
all: uai hello fbtest

uai: bin/uai
bin/uai: src/uai.c | bin
	$(CC) $(CFLAGS) -o $@ $<

hello: bin/hello
bin/hello: src/hello.c | bin
	$(CROSS) $(CROSSFLAGS) -o $@ $< -lm

fbtest: bin/fbtest
bin/fbtest: src/fbtest.c | bin
	$(CROSS) $(CROSSFLAGS) -o $@ $<

v4l2cap: bin/v4l2cap
bin/v4l2cap: src/v4l2cap.c | bin
	$(CROSS) $(CROSSFLAGS) -o $@ $<

bin:
	mkdir -p bin

deploy: all
	$(UAI) deploy bin/hello /tmp/hello

deploy-fb: fbtest
	$(UAI) deploy bin/fbtest /tmp/fbtest

monitor: uai
	$(UAI) monitor -t

term: uai
	$(UAI) term

clean:
	rm -f bin/uai bin/hello bin/fbtest
