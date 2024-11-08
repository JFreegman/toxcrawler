LIBS = toxcore
CFLAGS = -std=c17 -Wpedantic -Wunused -Wunused-macros -Wvla -Wmissing-declarations -Wall -Werror -ggdb -fstack-protector-all -pthread
OBJ = main.o util.o
LDFLAGS = $(shell pkg-config --libs $(LIBS))
SRC_DIR = ./src

all: $(OBJ)
	@echo "  LD    $@"
	@$(CC) $(CFLAGS) -o crawler $(OBJ) $(LDFLAGS)

%.o: $(SRC_DIR)/%.c
	@echo "  CC    $@"
	@$(CC) $(CFLAGS) -o $*.o -c $(SRC_DIR)/$*.c
	@$(CC) -MM $(CFLAGS) $(SRC_DIR)/$*.c > $*.d

clean:
	rm -f *.d *.o crawler

.PHONY: clean all
