#!/usr/bin/env bash
# Compare the private PHY functions our radio backend calls between two
# ESP-IDF refs (default: esp-sdr's pinned commit vs. the IDF we build with).
# Instruction streams are compared with addresses/relocation targets stripped.
# GPL-3.0-or-later.
#
# usage: tools/check_phy_abi.sh <chip> <reference-idf-ref> <candidate-idf-ref> [objdump]
#   e.g. tools/check_phy_abi.sh esp32s3 25fe69f946311abdaf9ad56591f25fedbc20ac98 v5.5.5
set -euo pipefail
chip=${1:?chip}; ref=${2:?reference ref}; cand=${3:?candidate ref}
objdump=${4:-}
if [[ -z $objdump ]]; then
    case $chip in
        esp32s3) objdump=$(ls ~/.platformio/packages/toolchain-xtensa-esp-elf*/bin/xtensa-esp32s3-elf-objdump | head -1) ;;
        *) objdump=$(ls ~/.platformio/packages/toolchain-riscv32-esp*/bin/riscv32-esp-elf-objdump | head -1) ;;
    esac
fi
case $chip in
    esp32s3) funcs="adctrig force_rx_gain set_chanfreq set_rf_freq_offset set_rx_gain_table set_rx_gain_cal_iq stop_tx_tone chip_v7_set_chan_ana phy_set_freq" ;;
    esp32c5) funcs="adctrig force_rx_gain phy_set_chanfreq phy_chip_set_chan_ana phy_set_rx_gain_cal_dc phy_set_rx_gain_cal_iq phy_adc_rate_cal_rxdc phy_set_rx_gain_table phy_stop_tx_tone phy_pbus_workmode phy_pbus_xpd_rx_on phy_pbus_xpd_tx_off phy_set_rxclk_en phy_rx_filter_mode" ;;
    *) echo "no function list for $chip" >&2; exit 2 ;;
esac
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
fetch() { # ref dir
    local sha
    sha=$(curl -sf "https://api.github.com/repos/espressif/esp-idf/contents/components/esp_phy/lib?ref=$1" |
          python3 -c 'import json,sys; print(json.load(sys.stdin)["sha"])')
    mkdir -p "$2"
    for lib in libphy.a librftest.a; do
        curl -sfL -o "$2/$lib" "https://github.com/espressif/esp-phy-lib/raw/$sha/$chip/$lib"
    done
    echo "$1 -> esp-phy-lib $sha"
}
fetch "$ref" "$work/ref"
fetch "$cand" "$work/cand"
disasm() { # lib func
    "$objdump" -dr --disassemble="$2" "$1" 2>/dev/null |
        sed -nE 's/^ *[0-9a-f]+:\s+[0-9a-f ]+\s+(.*)$/\1/p' | sed -E 's/[0-9a-f]{6,}( <[^>]*>)?/ADDR/g'
}
status=0
for f in $funcs; do
    found=0
    for lib in libphy.a librftest.a; do
        a=$(disasm "$work/ref/$lib" "$f"); b=$(disasm "$work/cand/$lib" "$f")
        [[ -z $a && -z $b ]] && continue
        found=1
        if [[ "$a" == "$b" ]]; then echo "SAME  $f ($lib, $(wc -l <<<"$a") insns)"
        else echo "DIFF  $f ($lib)"; status=1; fi
    done
    [[ $found == 1 ]] || echo "n/a   $f (not in archives; ROM or wrapper)"
done
exit $status
