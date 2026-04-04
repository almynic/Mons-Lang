CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -Iinclude
LDFLAGS ?=

SRC := src/main.c src/lexer.c src/parser.c src/arena.c src/ast_print.c src/types.c src/eval.c
OBJ := $(SRC:.c=.o)
BIN := mons

.PHONY: all run clean

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS)

src/%.o: src/%.c include/ast.h include/lexer.h include/parser.h include/types.h include/eval.h
	$(CC) $(CFLAGS) -c $< -o $@

run: $(BIN)
	./$(BIN)

clean:
	rm -f $(OBJ) $(BIN)
