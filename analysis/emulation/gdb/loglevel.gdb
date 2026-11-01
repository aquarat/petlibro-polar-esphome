# RAM-only alternative to the 'loglevel' image patch: run the UNPATCHED copy (work/flash.bin)
# with GDB=1 scripts/run_qemu.sh work/flash.bin 60, then: gdb -batch -x gdb/connect.gdb -x gdb/loglevel.gdb
# SYSTEM_INFO_init @0x420081d0 calls esp_log_level_set("*", a1) at 0x4200820c with a1 = 0 (NONE)
# because the NVS 'sysInfo' blob is absent. At 0x42008204 (after `c.li a1,0`) force a1 = 5 (VERBOSE).
hbreak *0x42008204
commands
  silent
  printf "SYSTEM_INFO_init: esp_log_level_set(\"*\", %d) -> forcing 5 (VERBOSE)\n", $a1
  set $a1 = 5
  continue
end
continue
