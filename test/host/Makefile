# Host build of the two pure components, with the tests that pin
# their byte layouts. No ESP-IDF needed: `make -C test/host run`.
CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O1
COMPONENTS := ../../components

tests: tests.c $(COMPONENTS)/text_record/text_record.c $(COMPONENTS)/tag_emu/tag_emu.c
	$(CC) $(CFLAGS) -I$(COMPONENTS)/text_record -I$(COMPONENTS)/tag_emu -o $@ $^

.PHONY: run clean
run: tests
	./tests

clean:
	rm -f tests
