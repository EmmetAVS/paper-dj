# Starting Paper DJ at boot

QNX's Raspberry Pi image runs `/system/etc/startup/post_startup.sh` as root at the end of every boot
(after networking, audio and the camera's sensor framework are up). `/system` links to the writable
`/usr` partition, so one added line survives reboots.

```sh
sudo sh ~/paper-dj/install_autostart.sh            # add the line (run once)
sudo sh ~/paper-dj/install_autostart.sh --remove   # take it out again
```

The installer:
- inserts `sh /data/home/qnxuser/paper-dj/start.sh &   # Paper DJ` just before the script's final `exit 0`
- keeps the untouched original as `post_startup.sh.orig`
- does nothing if the line is already there

`start.sh` sets its own `PATH`, so it works with the boot script's bare environment. It starts
`dj_engine` as root, which lets the engine create `/dev/paperdj`. To change songs, or to start the
vision program too (`VISION_CMD=`), edit the top of `start.sh`. The next boot picks the change up,
or run `sh ~/paper-dj/start.sh` to restart now.

Check after a reboot:

```sh
cat /dev/paperdj                 # both decks, SCHED_FIFO 50, deadline misses
cat ~/paper-dj/logs/engine.log   # startup log
```
