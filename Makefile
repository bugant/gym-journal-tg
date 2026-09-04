FINAL_LIBS = -lpthread -lcurl -lpq

# Linux ARM needs -latomic at linking time.
uname_M := $(shell uname -m 2>/dev/null)
ifneq (,$(filter aarch64 armv%,$(uname_M)))
	FINAL_LIBS += -latomic
endif

CC      ?= cc
CFLAGS  = -g -O2 -Wall -W -std=c11
PG_CFLAGS = $(shell pkg-config --cflags libpq 2>/dev/null)
TEST_BIN = test_gymjournal

.PHONY: all test e2e e2e-verbose e2e-filter clean

all: gymjournal

gymjournal: botlib.c cJSON.c sds.c pg_wrap.c json_wrap.c \
            gymjournal.c gymjournal_bot.c
	$(CC) $(CFLAGS) $(PG_CFLAGS) \
    botlib.c cJSON.c sds.c json_wrap.c pg_wrap.c \
	    gymjournal.c gymjournal_bot.c \
	    -o gymjournal $(FINAL_LIBS)

test: $(TEST_BIN)
	./$(TEST_BIN)

# End-to-end tests: real bot binary vs a local mock Telegram server and a
# scratch PostgreSQL cluster. Requires a system PostgreSQL (initdb/pg_ctl/
# createdb/psql on PATH) and python3. See README "End-to-end tests".
E2E_PY = python3 tests/e2e/run_e2e.py

e2e: gymjournal
	$(E2E_PY) --bot ./gymjournal

e2e-verbose: gymjournal
	$(E2E_PY) --bot ./gymjournal --verbose

# Run only scenarios whose name contains $(FILTER), e.g.:
#   make e2e-filter FILTER=add
e2e-filter: gymjournal
	$(E2E_PY) --bot ./gymjournal --filter "$(FILTER)"

# Pure-layer test target: deliberately links NO curl, NO botlib, NO libpq.
$(TEST_BIN): gymjournal.c test_gymjournal.c sds.c
	$(CC) $(CFLAGS) \
	    gymjournal.c sds.c test_gymjournal.c \
	    -o $(TEST_BIN) -lpthread

clean:
	rm -f gymjournal $(TEST_BIN)
