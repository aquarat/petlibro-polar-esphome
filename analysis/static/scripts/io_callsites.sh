#!/bin/bash
# List every call site of the GPIO/AW9523B/ADC/LEDC/audio helpers in the Ghidra decompile
# (work/out/decomp_all.c from run_ghidra.sh), grouped by calling function.
D="$(dirname "$0")/../work/out/decomp_all.c"
for f in 'FUN_ram_4200d8a6[(]' 'FUN_ram_42045570[(]' 'gpio_set_level[(]' 'gpio_config[(]' 'gpio_set_intr_type[(]' \
         'gpio_isr_handler_add[(]' 'ledc_set_duty[(]' 'adc1_config_channel_atten[(]' 'adc2_config_channel_atten[(]' \
         'FUN_ram_4200d7e8[(]' 'i2c_param_config[(]'; do
  echo "== $f"
  awk -v pat="$f" '/^\/\/ ===== /{fn=$3" "$5} $0 ~ pat && !/^\/\/ =====/{gsub(/^ +/,""); print "  " fn ": " $0}' "$D" | sort | uniq -c
done
