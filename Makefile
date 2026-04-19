CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -Iinclude
LDFLAGS ?=

SRC := src/main.c src/lexer.c src/parser.c src/arena.c src/ast_print.c src/types.c src/eval.c \
	src/repl.c src/bytecode.c src/compile.c src/vm.c src/reflection.c
OBJ := $(SRC:.c=.o)
BIN := mons

.PHONY: all run clean test

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS)

src/%.o: src/%.c include/ast.h include/lexer.h include/parser.h include/types.h include/eval.h \
	include/repl.h include/bytecode.h include/compile.h include/vm.h include/reflection.h
	$(CC) $(CFLAGS) -c $< -o $@

run: $(BIN)
	./$(BIN)

test: $(BIN)
	./$(BIN) tests/smoke.mons
	./$(BIN) tests/stdlib_core.mons
	./$(BIN) tests/use_tree_ok.mons
	./$(BIN) tests/closure.mons
	./$(BIN) tests/trait_impl.mons
	./$(BIN) tests/generics_trait_bounds.mons
	! ./$(BIN) tests/use_missing.mons > /dev/null 2>&1
	! ./$(BIN) tests/use_tree_missing.mons > /dev/null 2>&1
	! ./$(BIN) tests/use_cycle_a.mons > /dev/null 2>&1
	! ./$(BIN) tests/use_cycle_tree_a.mons > /dev/null 2>&1
	./$(BIN) --reflect tests/smoke.mons > /dev/null
	./$(BIN) --vm-test

clean:
	rm -f $(OBJ) $(BIN)
