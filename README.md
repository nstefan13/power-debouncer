# powerfilter: software debounce for a flaky power button

A small root tool (plus a Magisk module) for Android phones whose power button
has gone bad, for example after water damage or corrosion. It ignores the
"bounce" where the switch reports a release and a new press within a few
milliseconds of a single real press.

Tested on a: Motorola phone with Snapdragon; the power key was on `/dev/input/event0`
(device name `qpnp_pon`). 

It should work on any rooted Android phone with a Linux
input device and `/dev/uinput`, but you must check your own device name.

## The problem

A worn switch can open and close by itself for a few milliseconds. The kernel
then reports:

```
POWER DOWN
POWER UP      (8 ms later)
POWER DOWN    (17 ms later)
POWER UP
```

for one physical press. Android sees two presses. This could manifest as the screen needing several presses to turn off, the screenshot chord (Power + Volume Down) not working properly etc.

## How it works

```
real device (/dev/input/event0)  ->  powerfilter  ->  virtual device (uinput)  ->  Android
```

1. It creates a virtual input device that copies the real one (same name, same keys).
2. It grabs the real device (`EVIOCGRAB`), so only powerfilter receives the raw events.
3. Every event is forwarded unchanged, except the power key release, which is
   held back for `debounce_ms`:
   - If the key is pressed again inside that window, it was a bounce. The
     release and the new press are both dropped, so Android sees one clean press.
   - If nothing happens, the release is forwarded after the delay.

## Requirements

- Root (Magisk or similar)
- `/dev/uinput` available and not blocked by SELinux
- To build: Termux (`pkg install clang zip`) or a PC with the Android NDK

# Building & Installation

## Find your power key device

In a root shell:

```
getevent -lp
```

Find the device that lists `KEY_POWER`. On the test phone it is:

```
add device 11: /dev/input/event0
  name:     "qpnp_pon"
  events:
    KEY (0001): KEY_VOLUMEDOWN        KEY_POWER
```

Note the **name**. Another device may also list `KEY_POWER` (touchscreens
sometimes do); the real button is the one that produces events when you press it.

## Measure your bounce first

With the filter stopped, in a root shell:

```
getevent -lt /dev/input/event0
```

Press the power button once. A clean press shows one DOWN and one UP. A bounce
shows DOWN, UP, DOWN, UP a few milliseconds apart. The largest UP-to-DOWN gap
you see is the minimum debounce value; add margin. Example from the test phone:

```
2407.699  KEY_POWER DOWN
2407.707  KEY_POWER UP      (+8 ms)
2407.724  KEY_POWER DOWN    (+17 ms gap)
2407.739  KEY_POWER UP
```

Gaps of about 20 ms, so 70 ms leaves roughly 3x margin. Do not go much higher:
a real double-press has a gap of about 80 to 150 ms and would be merged too.

## Build

You have 2 ways to build it: either on your phone using Termux, or on your PC using NDK (part of the Android SDK).

## Option 1: Build on mobile with Termux

```
pkg install clang zip
sh build.sh
```

## Option 2: Build on PC with the NDK

```
NDK=/path/to/android-ndk sh build.sh
```

This produces `module/bin/powerfilter` and `powerfilter-magisk.zip`.

## (optional) Try it manually first 

From `adb shell` so you can always stop it:

```
adb push module/bin/powerfilter /data/local/tmp/
adb shell
su
chmod 755 /data/local/tmp/powerfilter
/data/local/tmp/powerfilter -n qpnp_pon 70 -v
```

Usage:

```
powerfilter [/dev/input/eventN | -n devname] [debounce_ms] [-v]
```

| Option | Meaning |
|---|---|
| `/dev/input/eventN` | use this device path (default `/dev/input/event0`) |
| `-n devname` | find the device by name; survives event number changes between boots |
| `debounce_ms` | hold window in milliseconds (default 70) |
| `-v` | log every power event with timestamps and bounce gaps |

Example `-v` output for a clean press:

```
[2741.101] POWER DOWN  -> forwarded
[2741.129] POWER UP    -> held back for up to 70 ms
[2741.199] POWER UP    -> no bounce within 70 ms, release forwarded
```

and for a bounce:

```
[2504.552] POWER DOWN  -> 22 ms after release: BOUNCE, ignored
```

Stop it with Ctrl+C or `su -c pkill powerfilter`.

## Install as a Magisk module

1. Build (above) to get `powerfilter-magisk.zip`.
2. Magisk app, Modules, Install from storage, pick the zip.
3. Reboot.

On every boot `service.sh` starts powerfilter in the background in a restart
loop, finding the device by the name `qpnp_pon`. Log file:
`/data/local/tmp/powerfilter.log`.

To use a different device name or debounce time, edit `DEVICE_NAME` and
`DELAY_MS` at the top of `module/service.sh` before building.

## Stopping and reverting

Nothing is persistent. If powerfilter ends for any reason (Ctrl+C, `kill`,
`kill -9`, a crash, a reboot), the kernel releases the grab and removes the
virtual device, and the real button works as before. On a normal stop it also
releases the power key if it was held, so it can't stay stuck down.

| Goal | Command |
|---|---|
| Stop now | `su -c pkill powerfilter` |
| Stop and keep stopped | `su -c "touch /data/adb/powerfilter.disable; pkill powerfilter"` |
| Re-enable | `su -c rm /data/adb/powerfilter.disable`, then reboot |
| Remove completely | uninstall the module in Magisk, or delete `/data/adb/modules/powerfilter` from recovery |

The filter runs after boot in the background and does not block startup, so it
should not cause a bootloop.

## Debugging

- **Power button does nothing with the filter running.** Read the virtual
  device to see whether events leave the filter: run `getevent -lp | grep -B1 qpnp_pon`,
  note the second `qpnp_pon` (the new event number), then `getevent -lt /dev/input/eventN`.
  Also compare both entries in `dumpsys input | grep -A12 qpnp_pon`: the virtual
  one should have a keyboard class and a key layout file. If its layout is empty,
  Android is not mapping the key to power.
- **Volume down works but power does not:** likely a key layout mismatch for
  the virtual device, as above.
- **Fails to start (`open uinput` error):** SELinux may be blocking it. Check
  with `setenforce 0` temporarily to confirm, then look at `dmesg | grep avc`.
- **Bounces still get through:** raise `debounce_ms` (try 100 or 150).
- **Button feels laggy on release:** lower `debounce_ms`.
- **Screenshot chord:** hold Power and Volume Down together for about a second.
  Tapping volume down, releasing, then pressing power will not work.

## Limitations

- It fixes bounce only. If the contact fails to register a press at all, no
  event exists and software can't help.
- It filters only `KEY_POWER` on the chosen device. Volume down on the same
  device passes through untouched.
- Releases are delayed by the debounce time, which is the price of filtering.
- Not tested across ROMs. Some may need a different device name or a SELinux
  policy change for `/dev/uinput`.

## Files

| File | Purpose |
|---|---|
| `powerfilter.c` | the filter (single C file, no dependencies) |
| `build.sh` | compiles it and creates the Magisk zip |
| `module/` | Magisk module (`module.prop`, `service.sh`, `customize.sh`, installer) |

Water damage may keep spreading. Cleaning the button area with 99% isopropyl
alcohol is worth doing before the corrosion reaches the board.
