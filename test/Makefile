#---------------------------------------------------------------------------------
# Host-native unit test build for maxtracker.
#
# Compiles pure-logic modules with the system's gcc (NOT devkitARM).
# Uses nds_shim.h to provide NDS type definitions.
#
# Usage: make -C test   (or "make host-test" from top level)
#---------------------------------------------------------------------------------

CC      := gcc
CFLAGS  := -Wall -Wextra -g -O0 \
           -I$(CURDIR)/nds \
           -I$(CURDIR) \
           -I$(CURDIR)/../arm9/source/core \
           -I$(CURDIR)/../arm9/source/ui \
           -I$(CURDIR)/../arm9/source/io \
           -I$(CURDIR)/../arm9/source/test \
           -I$(CURDIR)/../include \
           -I$(CURDIR)/../lib/maxmod/include

# Source files under test (pure logic, no NDS hardware)
ARM9_CORE := ../arm9/source/core
ARM9_IO   := ../arm9/source/io
ARM9_UI   := ../arm9/source/ui
ARM9_TEST := ../arm9/source/test

SRCS := host_main.c \
        memtrack_stub.c \
        $(ARM9_TEST)/test.c \
        $(ARM9_CORE)/song.c \
        $(ARM9_CORE)/clipboard.c \
        $(ARM9_CORE)/undo.c \
        $(ARM9_CORE)/scale.c \
        $(ARM9_IO)/mas_write.c \
        $(ARM9_IO)/mas_load.c \
        $(ARM9_UI)/editor_state.c

TARGET := maxtracker_tests

# ---- Playback comparison test (compiles the maxmod engine on host) ----

ENGINE_CFLAGS := -Wall -Wextra -g -O0 \
           -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast \
           -include $(CURDIR)/mm_engine_shim.h \
           '-Dtarget(x)=' \
           -I$(CURDIR) -I$(CURDIR)/nds \
           -I$(CURDIR)/../arm9/source/core \
           -I$(CURDIR)/../arm9/source/io \
           -I$(CURDIR)/../arm9/source/test \
           -I$(CURDIR)/../include \
           -I$(CURDIR)/../lib/maxmod/include \
           -I$(CURDIR)/../lib/maxmod/source

MM_ENGINE := ../lib/maxmod/source/core/mas.c \
             ../lib/maxmod/source/core/mas_arm.c

PLAYBACK_CMP_SRCS := playback_cmp.c engine_stubs.c \
                      $(MM_ENGINE) \
                      $(ARM9_CORE)/song.c \
                      $(ARM9_CORE)/scale.c \
                      $(ARM9_IO)/mas_load.c \
                      $(ARM9_IO)/mas_write.c

PLAYBACK_CMP := playback_cmp

# ---- Reader comparison test ----

READER_CMP_SRCS := reader_cmp.c engine_stubs.c \
                   $(ARM9_CORE)/song.c \
                   $(ARM9_CORE)/scale.c \
                   $(ARM9_IO)/mas_load.c \
                   $(ARM9_IO)/mas_write.c

READER_CMP := reader_cmp

# ---- MAS roundtrip diff ----

MAS_DIFF_SRCS := mas_diff.c \
                 memtrack_stub.c \
                 $(ARM9_CORE)/song.c \
                 $(ARM9_CORE)/scale.c \
                 $(ARM9_IO)/mas_write.c \
                 $(ARM9_IO)/mas_load.c

MAS_DIFF := mas_diff

# ---- WAV loader tests (round-trip synthetic .wav files) ----

WAV_TEST_SRCS := wav_test.c $(ARM9_IO)/wav_load.c

WAV_TEST := wav_test

# ---- Filebrowser tests (directory parsing, sort, at_root regression) ----

FB_TEST_SRCS := filebrowser_test.c $(ARM9_IO)/filebrowser.c $(ARM9_UI)/scroll_view.c

FB_TEST := filebrowser_test

# ---- Navigation tests (SHIFT-chord dispatcher contract) ----
#
# Links the real navigation.c + editor_state.c + song.c; the UI side
# (screen, font, sample_view, LFE) is stubbed in navigation_test.c.
# Intentionally built WITHOUT -DMAXTRACKER_LFE so the LFE transition
# branches are excluded and we don't need stubs for them.

NAV_TEST_SRCS := navigation_test.c \
                 $(ARM9_UI)/navigation.c \
                 $(ARM9_UI)/editor_state.c \
                 $(ARM9_CORE)/song.c \
                 $(ARM9_CORE)/scale.c

NAV_TEST := navigation_test

# ---- Targets ----

.PHONY: all clean run run-playback-cmp

all: $(TARGET) $(PLAYBACK_CMP) $(READER_CMP) $(MAS_DIFF) $(WAV_TEST) $(FB_TEST) $(NAV_TEST)

$(TARGET): $(SRCS) nds_shim.h
	$(CC) $(CFLAGS) -o $@ $(SRCS)

$(PLAYBACK_CMP): $(PLAYBACK_CMP_SRCS) mm_engine_shim.h nds_shim.h
	$(CC) $(ENGINE_CFLAGS) -o $@ $(PLAYBACK_CMP_SRCS)

$(READER_CMP): $(READER_CMP_SRCS) nds_shim.h
	$(CC) $(ENGINE_CFLAGS) -o $@ $(READER_CMP_SRCS)

$(MAS_DIFF): $(MAS_DIFF_SRCS) nds_shim.h
	$(CC) $(CFLAGS) -o $@ $(MAS_DIFF_SRCS)

$(WAV_TEST): $(WAV_TEST_SRCS) nds_shim.h
	$(CC) $(CFLAGS) -o $@ $(WAV_TEST_SRCS) -lm

$(FB_TEST): $(FB_TEST_SRCS) nds_shim.h
	$(CC) $(CFLAGS) -o $@ $(FB_TEST_SRCS)

$(NAV_TEST): $(NAV_TEST_SRCS) nds_shim.h
	$(CC) $(CFLAGS) -o $@ $(NAV_TEST_SRCS)

run: $(TARGET) $(WAV_TEST) $(FB_TEST) $(NAV_TEST)
	./$(TARGET)
	./$(WAV_TEST)
	./$(FB_TEST)
	./$(NAV_TEST)

# Run playback comparison on all .mas files in data/BestOf
run-playback-cmp: $(PLAYBACK_CMP)
	@pass=0; fail=0; skip=0; \
	for f in ../data/BestOf/*.mas; do \
	  result=$$(./$(PLAYBACK_CMP) "$$f" 50000 2>&1); \
	  name=$$(basename "$$f"); \
	  if echo "$$result" | grep -q "^PASS"; then \
	    echo "PASS: $$name"; pass=$$((pass+1)); \
	  elif echo "$$result" | grep -q "^FAIL"; then \
	    echo "FAIL: $$name"; echo "$$result" | grep "^TICK" | head -3; fail=$$((fail+1)); \
	  else \
	    echo "SKIP: $$name"; skip=$$((skip+1)); \
	  fi; \
	done; \
	echo ""; echo "Results: $$pass PASS, $$fail FAIL, $$skip SKIP"; \
	test $$fail -eq 0

clean:
	rm -f $(TARGET) $(TARGET).exe \
	      $(PLAYBACK_CMP) $(PLAYBACK_CMP).exe \
	      $(READER_CMP) $(READER_CMP).exe \
	      $(MAS_DIFF) $(MAS_DIFF).exe \
	      $(WAV_TEST) $(WAV_TEST).exe \
	      $(FB_TEST) $(FB_TEST).exe \
	      $(NAV_TEST) $(NAV_TEST).exe \
	      wav_test_tmp.wav \
	      _test_autosave.mas
	rm -rf fb_test_root
