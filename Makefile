GUEST_ASM = src/boot.asm
GUEST_BIN = payload/guest.bin

.PHONY: build run clean

build: $(GUEST_BIN)
	cargo build

run: build
	cargo run

$(GUEST_BIN): $(GUEST_ASM)
	nasm -f bin -o $@ $<

clean:
	rm -f $(GUEST_BIN)
	cargo clean
