#!/bin/sh
# MY-FAM by David Amo (Miyoo Mini Plus, Allium): /mnt/SDCARD/Apps/MY-FAM.pak/launch.sh
# (the program is my-fam.elf: an extension, so FTP clients send it as binary, not as text)
cd "$(dirname "$0")"
DIR=$(pwd)
TMP_UPDATE=/mnt/SDCARD/.tmp_update

# Allium's own library path, as it is: its SDL 1.2 from /customer/lib. (Not .tmp_update/lib's: that one draws a red
# battery over the picture.) That SDL does not turn the picture (MY-FAM does) and plays through Allium's audioserver:
# SDL's OSS driver, with libpadsp in front.
export LD_LIBRARY_PATH="$DIR/lib:$LD_LIBRARY_PATH"
export SDL_AUDIODRIVER=dsp
[ -f /customer/lib/libpadsp.so ] && export LD_PRELOAD=/customer/lib/libpadsp.so
export HOME=/mnt/SDCARD
export MYFAM_SAVE="$DIR/my-fam.flash"
# settings.sh (optional): MYFAM_WAV_ROOT=/mnt/SDCARD/Music (where the WAV folder picker starts), MYFAM_ROTATE=0 (the picture not turned), MYFAM_AHEAD=6144 (more sound ahead, if it
# crackles), MYFAM_FPS=20, MYFAM_STATS=0, MYFAM_SPLASH=0 (no start screen)
[ -f "$DIR/settings.sh" ] && . "$DIR/settings.sh"

echo performance > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null
[ -f "$TMP_UPDATE/script/set_sound_level.sh" ] && "$TMP_UPDATE/script/set_sound_level.sh" &

LOG="$DIR/log.txt"
{
    echo "launch.sh: MY-FAM by David Amo, $(date) my-fam.elf $(wc -c < ./my-fam.elf) bytes $(md5sum ./my-fam.elf 2>/dev/null | cut -c1-32)"
    if [ -f my-fam.md5 ]; then
        [ "$(md5sum ./my-fam.elf 2>/dev/null | cut -c1-32)" = "$(cut -c1-32 my-fam.md5)" ] && echo "launch.sh: my-fam.elf checksum OK" \
            || echo "launch.sh: my-fam.elf checksum WRONG: damaged on the way (FTP in ASCII mode?), copy it again in binary mode"
    fi
    echo "launch.sh: LD_LIBRARY_PATH=$LD_LIBRARY_PATH"
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

cat /dev/zero > /dev/fb0 2>/dev/null
echo ondemand > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null
