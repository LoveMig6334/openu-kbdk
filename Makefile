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

# Allwinner MPP (eyesee-mpp) build flags for the camera path. Headers are
# vendored from the sun8iw19p1 (V831) Tina SDK; symbols are resolved at runtime
# via dlopen against the board's /usr/lib/eyesee-mpp/*.so, so nothing to link.
# AWCHIP=0x1817 (AW_V459) is the V83x family value; it only affects log strings.
MPPDIR  := vendor/eyesee-mpp/sun8iw19p1/include
MPPINC  := -I$(MPPDIR) -I$(MPPDIR)/media -I$(MPPDIR)/utils
MPPDEF  := -DAWCHIP=0x1817

UAI := ./bin/uai

.PHONY: all uai hello fbtest audio v4l2cap v4l2probe camdiag camread cammpp campreview camcc nncls clean \
        deploy deploy-fb deploy-audio deploy-cammpp deploy-preview preview-start preview-stop \
        deploy-camcc camcc-start camcc-stop deploy-nncls nnaprobe deploy-nnaprobe monitor term
all: uai hello fbtest audio

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

audio: bin/audio
bin/audio: src/audio.c | bin
	$(CROSS) $(CROSSFLAGS) -o $@ $< -lm

# Camera diagnostics (V4L2 recon — proved standard streaming is unavailable here)
v4l2probe: bin/v4l2probe
bin/v4l2probe: src/v4l2probe.c | bin
	$(CROSS) $(CROSSFLAGS) -o $@ $<
camdiag: bin/camdiag
bin/camdiag: src/camdiag.c | bin
	$(CROSS) $(CROSSFLAGS) -o $@ $<
camread: bin/camread
bin/camread: src/camread.c | bin
	$(CROSS) $(CROSSFLAGS) -o $@ $<

# Camera capture via Allwinner MPP (the working path). Resolves AW_MPI_* at
# runtime through dlopen, so link only -ldl/-lpthread (no vendor .so on host).
cammpp: bin/cammpp
bin/cammpp: src/cammpp.c | bin
	$(CROSS) $(CROSSFLAGS) $(MPPDEF) $(MPPINC) -o $@ $< -ldl -lpthread

# Live camera preview: VI->VO hardware bind (camera straight to the LCD)
campreview: bin/campreview
bin/campreview: src/campreview.c | bin
	$(CROSS) $(CROSSFLAGS) $(MPPDEF) $(MPPINC) -o $@ $< -ldl -lpthread

# Color-corrected live preview: MPP capture -> CPU white-balance/saturation -> fb0
camcc: bin/camcc
bin/camcc: src/camcc.c | bin
	$(CROSS) $(CROSSFLAGS) $(MPPDEF) $(MPPINC) -o $@ $< -ldl -lpthread

# NPU CNN inference: dlopen the board's libmaix_nn.so (AWNN), run forward
nncls: bin/nncls
bin/nncls: src/nncls.c | bin
	$(CROSS) $(CROSSFLAGS) -Ivendor/libmaix -Wl,--export-dynamic -o $@ $< -ldl

# NPU userspace bring-up: read-only probe of the NVDLA regs via /dev/mem (no driver)
nnaprobe: bin/nnaprobe
bin/nnaprobe: src/nnaprobe.c | bin
	$(CROSS) $(CROSSFLAGS) -o $@ $<

deploy-nnaprobe: nnaprobe
	$(UAI) push bin/nnaprobe /tmp/nnaprobe && $(UAI) exec "chmod +x /tmp/nnaprobe"
	@echo 'run: ./bin/uai exec "/tmp/nnaprobe"'

bin:
	mkdir -p bin

deploy: all
	$(UAI) deploy bin/hello /tmp/hello

deploy-fb: fbtest
	$(UAI) deploy bin/fbtest /tmp/fbtest

deploy-audio: audio
	$(UAI) push bin/audio /tmp/audio && $(UAI) exec "chmod +x /tmp/audio"

deploy-cammpp: cammpp
	$(UAI) push bin/cammpp /tmp/cammpp && $(UAI) exec "chmod +x /tmp/cammpp"
	@echo 'run: ./bin/uai exec "LD_LIBRARY_PATH=/usr/lib/eyesee-mpp:/usr/lib /tmp/cammpp 320x240"'

deploy-preview: campreview
	$(UAI) push bin/campreview /tmp/campreview && $(UAI) exec "chmod +x /tmp/campreview"
	@echo 'start live preview: make preview-start   |   stop: make preview-stop'

# Start the live camera preview detached (runs until stopped). MUST background in
# a subshell and redirect output, or the MPP logs flood the serial console and
# the trailing & breaks uai's exec marker.
preview-start: deploy-preview
	$(UAI) exec "(LD_LIBRARY_PATH=/usr/lib/eyesee-mpp:/usr/lib /tmp/campreview 320x240 0 >/tmp/preview.log 2>&1 &); echo started"
	@echo 'live camera should be on the panel; stop with: make preview-stop'

preview-stop:
	$(UAI) exec "kill \$$(pidof campreview) 2>/dev/null; echo stopping"

deploy-camcc: camcc
	$(UAI) push bin/camcc /tmp/camcc && $(UAI) exec "chmod +x /tmp/camcc"
	@echo 'start corrected preview: make camcc-start   |   stop: make camcc-stop'

# Color-corrected live preview, detached (backgrounded in a subshell + log redirect
# so MPP's verbose logs don't flood serial / break uai's exec marker).
camcc-start: deploy-camcc
	$(UAI) exec "(LD_LIBRARY_PATH=/usr/lib/eyesee-mpp:/usr/lib /tmp/camcc 320x240 0 9 -11 1.6 0 >/tmp/camcc.log 2>&1 &); echo started"
	@echo 'corrected camera should be on the panel; stop with: make camcc-stop'

camcc-stop:
	$(UAI) exec "kill \$$(pidof camcc) 2>/dev/null; echo stopping"

deploy-nncls: nncls
	$(UAI) push bin/nncls /tmp/nncls && $(UAI) exec "chmod +x /tmp/nncls"
	@echo 'run: ./bin/uai exec "LD_LIBRARY_PATH=/usr/lib/eyesee-mpp:/usr/lib /tmp/nncls"'

monitor: uai
	$(UAI) monitor -t

term: uai
	$(UAI) term

clean:
	rm -f bin/uai bin/hello bin/fbtest bin/audio \
	      bin/v4l2cap bin/v4l2probe bin/camdiag bin/camread bin/cammpp bin/campreview \
	      bin/camcc bin/nncls bin/nnaprobe
