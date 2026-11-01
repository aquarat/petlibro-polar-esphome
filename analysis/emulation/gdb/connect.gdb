# Connect to QEMU's gdb stub (run_qemu.sh with GDB=1). No ELF symbols: the app is analysed by address.
set pagination off
set confirm off
set architecture riscv:rv32
target remote :1234
