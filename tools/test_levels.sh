#!/bin/bash
# Plays each course from the 100% dev save (tools/devsave/pokemonsnap_100.bin): menu -> course N,
# zoom + photos, Pester Balls, then Camera Check. Per course: snapshots and the log in
# tools/tmp/levels/<n>_<name>/. Usage: bash tools/test_levels.sh [course indexes...] (default 0-6)
cd "$(dirname "$0")/.."
names=(beach tunnel volcano river cave valley rainbow)
sd=azahar/user/sdmc/3ds/psnap
courses=("$@"); [ ${#courses[@]} -eq 0 ] && courses=(0 1 2 3 4 5 6)
for n in "${courses[@]}"; do
    out=tools/tmp/levels/${n}_${names[$n]}; mkdir -p $out; rm -f $out/*
    script=$out/autoinput.txt
    { echo "1300 400 START TAP:40"; echo "1750 20 A"; echo "2100 20 A"; echo "2500 20 A"
      for ((k = 0; k < n; k++)); do echo "$((2800 + k * 100)) 20 STICK:0,-120"; done
      echo "3300 20 A"; echo "3500 20 A"; echo "3700 20 A"
      echo "4600 4000 ZR"; echo "4700 3900 A TAP:90"
      echo "8800 20 B"; echo "9100 20 B"; echo "9400 20 B"; echo "9700 20 B"; } > $script
    seq 4200 800 13000 > $sd/snapshots.txt
    rm -f $sd/snap_*.bmp
    powershell -ExecutionPolicy Bypass -File tools/test_run.ps1 -Seconds 300 -SampleEvery 100 -LogLines 0 \
        -Script $script -Save tools/devsave/pokemonsnap_100.bin > $out/run.txt
    rm -f $sd/snapshots.txt
    mv $sd/snap_*.bmp $out/ 2>/dev/null
    cp $sd/runtime.log $out/runtime.log
    echo "${names[$n]} done"
done
