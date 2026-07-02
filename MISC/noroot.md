1. Enable console autologin in the Buildroot config.

In `buildroot/.config` set:
```make
BR2_TARGET_GENERIC_GETTY_OPTIONS="-n -l /bin/sh"
BR2_ROOTFS_OVERLAY="board/trisul32/rootfs-overlay"
```

If they also keep a saved config copy, make the same change in `buildroot/current.config`:
```make
BR2_TARGET_GENERIC_GETTY_OPTIONS="-n -l /bin/sh"
BR2_ROOTFS_OVERLAY="board/trisul32/rootfs-overlay"
```

2. Create the rootfs overlay directories:

```sh
mkdir -p buildroot/board/trisul32/rootfs-overlay/etc/init.d
mkdir -p buildroot/board/trisul32/rootfs-overlay/usr/bin
```

3. Create `buildroot/board/trisul32/rootfs-overlay/etc/init.d/S99boot-echo` with:

```sh
#!/bin/sh

case "$1" in
start)
	/usr/bin/boot_echo.sh
	;;
stop|restart|reload)
	;;
*)
	echo "Usage: $0 start" >&2
	exit 1
	;;
esac
```

4. Create `buildroot/board/trisul32/rootfs-overlay/usr/bin/boot_echo.sh` with:

```sh
#!/bin/sh

MESSAGE="boot_echo.sh: boot completed"

echo "$MESSAGE" > /dev/console
echo "$MESSAGE" >> /root/boot.log
```

5. Make both scripts executable:

```sh
chmod 755 buildroot/board/trisul32/rootfs-overlay/etc/init.d/S99boot-echo
chmod 755 buildroot/board/trisul32/rootfs-overlay/usr/bin/boot_echo.sh
```

6. Rebuild:

```sh
./build.sh
```

What this does:
- Boot goes straight to a root shell without asking for a password.
- On every boot, `boot_echo.sh` runs automatically.
- You can replace the `echo` lines in `boot_echo.sh` with any program you want to launch at boot, using absolute paths.

One caution to pass along:
- `-n -l /bin/sh` skips login entirely and drops directly into a root shell, so this is good for development boards but not safe for production.
