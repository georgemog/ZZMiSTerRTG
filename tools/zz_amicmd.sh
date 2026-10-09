#!/bin/sh
# usage: zz_amicmd.sh <seconds> "cmd1" "cmd2" ...   -> runs on Amiga serial CLI
T=$1; shift
stty -F /dev/ttyS1 115200 raw -echo -crtscts clocal
OUT=/media/fat/logs/zz_ser.out
timeout $T cat /dev/ttyS1 > $OUT &
sleep 0.3
for c in "$@"; do printf "%s\r\n" "$c" > /dev/ttyS1; sleep 0.3; done
wait
tr -d "\017" < $OUT | sed "s/\x9b[0-9;]*[a-zA-Z]//g; s/\x1b\[[0-9;]*[a-zA-Z]//g; s/\r//g"
