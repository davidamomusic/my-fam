#!/bin/sh
# MY-FAM by David Amo, for the Miyoo Mini Plus: the same launch.sh in Apps/MY-FAM.pak (Allium) and App/MY-FAM
# (OnionOS, Miyoo firmware). Which system it runs on, from what is on the SD card (Allium also has .tmp_update):
if [ -d /mnt/SDCARD/.allium ]; then MYFAM_OS=allium
elif [ -e /mnt/SDCARD/.tmp_update/onionVersion ]; then MYFAM_OS=onion
else MYFAM_OS=stock; fi
# (the program is my-fam.elf: an extension, so FTP clients send it as binary, not as text)
cd "$(dirname "$0")"
DIR=$(pwd)

# The library path as the system gives it: the Miyoo's own SDL 1.2 (/customer/lib, or Onion's copy of it in
# /mnt/SDCARD/miyoo/lib). It does not turn the picture (MY-FAM does) and plays through the Miyoo's audioserver: SDL's
# OSS driver with libpadsp in front. (On Allium not .tmp_update/lib's SDL: that one draws a red battery.)
[ -n "$LD_LIBRARY_PATH" ] || LD_LIBRARY_PATH=/lib:/config/lib:/customer/lib
export LD_LIBRARY_PATH="$DIR/lib:$LD_LIBRARY_PATH"
export SDL_AUDIODRIVER=dsp
PADSP=/customer/lib/libpadsp.so
case "$MYFAM_OS" in
onion) [ -f /mnt/SDCARD/miyoo/lib/libpadsp.so ] && PADSP=/mnt/SDCARD/miyoo/lib/libpadsp.so ;;
stock) [ -f /mnt/SDCARD/miyoo354/lib/libpadsp.so ] && PADSP=/mnt/SDCARD/miyoo354/lib/libpadsp.so ;;
esac
[ -f "$PADSP" ] && export LD_PRELOAD="$PADSP"
export HOME=/mnt/SDCARD
export MYFAM_SAVE="$DIR/my-fam.flash"
# settings.sh (optional): MYFAM_WAV_ROOT=/mnt/SDCARD/Music (where the WAV folder picker starts), MYFAM_ROTATE=0 (the picture not turned), MYFAM_AHEAD=6144 (more sound ahead, if it
# crackles), MYFAM_FPS=20, MYFAM_STATS=0, MYFAM_SPLASH=0 (no start screen)
[ -f "$DIR/settings.sh" ] && . "$DIR/settings.sh"

echo performance > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null
case "$MYFAM_OS" in
allium) [ -f /mnt/SDCARD/.tmp_update/script/set_sound_level.sh ] && /mnt/SDCARD/.tmp_update/script/set_sound_level.sh & ;;
onion) touch /tmp/stay_awake ;;     # (no sleeping while a pattern plays)
esac

LOG="$DIR/log.txt"
{
    echo "launch.sh: MY-FAM by David Amo ($MYFAM_OS), $(date) my-fam.elf $(wc -c < ./my-fam.elf) bytes $(md5sum ./my-fam.elf 2>/dev/null | cut -c1-32)"
    if [ -f my-fam.md5 ]; then
        [ "$(md5sum ./my-fam.elf 2>/dev/null | cut -c1-32)" = "$(cut -c1-32 my-fam.md5)" ] && echo "launch.sh: my-fam.elf checksum OK" \
            || echo "launch.sh: my-fam.elf checksum WRONG: damaged on the way (FTP in ASCII mode?), copy it again in binary mode"
    fi
    echo "launch.sh: LD_LIBRARY_PATH=$LD_LIBRARY_PATH LD_PRELOAD=$LD_PRELOAD"
    [ -f "$MYFAM_SAVE" ] && echo "launch.sh: save file $(wc -c < "$MYFAM_SAVE") bytes" || echo "launch.sh: no save file"
} > "$LOG" 2>&1
./my-fam.elf >> "$LOG" 2>&1
rc=$?
echo "launch.sh: my-fam.elf exited with $rc" >> "$LOG"
# a crash with a save file: once more without it (kept as my-fam.flash.bad), to tell the two apart
if [ $rc -ge 128 ] && [ -f "$MYFAM_SAVE" ]; then
    mv -f "$MYFAM_SAVE" "$MYFAM_SAVE.bad"
    echo "launch.sh: save file moved to my-fam.flash.bad, trying again without it" >> "$LOG"
    ./my-fam.elf >> "$LOG" 2>&1
    echo "launch.sh: my-fam.elf exited with $?" >> "$LOG"
fi
sync

[ "$MYFAM_OS" = onion ] && rm -f /tmp/stay_awake
cat /dev/zero > /dev/fb0 2>/dev/null
echo ondemand > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null
