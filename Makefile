# Custom Shell - Operating System I group project, Group 6
#
#   make          build every command, then the shell
#   make test     run every command's test suite
#   make clean    remove all build output

COMMAND_DIRS := $(patsubst %/Makefile,%,$(wildcard */Makefile))
BUILD_DIRS   := $(filter-out _template,$(COMMAND_DIRS))

all:
	@for d in $(BUILD_DIRS); do \
	    echo "==> $$d"; \
	    $(MAKE) --no-print-directory -C $$d || exit 1; \
	done

test:
	@for d in $(BUILD_DIRS); do \
	    if [ -x $$d/tests/run_tests.sh ]; then \
	        echo "==> $$d tests"; \
	        ( cd $$d && ./tests/run_tests.sh ) || exit 1; \
	    fi; \
	done

clean:
	@for d in $(BUILD_DIRS); do $(MAKE) --no-print-directory -C $$d clean; done

.PHONY: all test clean
