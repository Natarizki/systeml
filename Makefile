CC      = cc
CFLAGS  = -Wall -Wextra -O2 -std=c11
SRC     = src/main.c src/service.c src/spawn.c src/pid1.c src/control.c
OBJ     = $(SRC:.c=.o)
BIN     = systeml
CTL_BIN = systemlctl

.PHONY: all clean

all: $(BIN) $(CTL_BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $(BIN) $(OBJ)

$(CTL_BIN): src/systemlctl.c
	$(CC) $(CFLAGS) -o $(CTL_BIN) src/systemlctl.c

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(BIN) $(CTL_BIN)
