# Architecture

This file describes how the app runs the iPod software on the Innioasis Y1.
It also describes the main concepts and explains which parts run
natively and which parts the app emulates. 

## The idea

The iPod 5G and 5.5G contain a PortalPlayer PP5022 chip. This chip has two
ARM7TDMI cores: the CPU and the COP. The iPod also has 64 MB of SDRAM,
128 KB of fast internal RAM (IRAM), and a Broadcom BCM2722 video chip for
the screen. The firmware, is ARM code (ARMv4T, ARM state) for
these cores.

The Innioasis Y1 has an ARMv7 processor (MT6572, Cortex-A7). An ARMv7
processor also runs the ARM instructions of the older cores. Therefore the
Y1 runs the firmware instructions directly, without translation. A full
emulator (QEMU) translates each block of guest code. On the 1 GHz Y1, QEMU
needed more than 5 minutes for the first frame. The native runner shows the
first frame after about 18 s. It plays music in real time.

The runner emulates only what the Y1 cannot do natively for the firmware:

- the privileged instructions (the firmware expects to be the operating
  system, but the runner is a normal Linux process),
- the iPod devices (registers at fixed addresses),
- the interrupts, the second core, and the time.

## Overview

```
 Innioasis Y1 (MT6572, Android 4.2)
 +-----------------------------------------------------------------------+
 | App "iPod (native)" (Java)              runner-native (C, as root)    |
 |                                                                       |
 | StartActivity                           retailOS code (the firmware): |
 |  - IPSW -> firmware.bin, osos.bin  ---> runs natively                 |
 |  - Build database -> iTunesDB,          signal handlers: privileged   |
 |    ArtworkDB, .ithmb, manifest          instructions, devices, IRQs   |
 |                                         device models (PP5022,        |
 | NativeActivity                          BCM2722, codec, power chip)   |
 |  - keys, battery  --- stdin ----------> click wheel, GPIO, PMU        |
 |  - AudioTrack    <--- stdout (PCM) ---- DMA + I2S                     |
 |  - SurfaceView   <--- frames.fifo ----- BCM2722 screen, events        |
 |  - brightness, volume <- (events) ----- backlight, codec              |
 |  - SIGTERM (HOME) ------------------->  hibernate: snap.bin           |
 |                                                                       |
 | /sdcard/retailos/disk, manifest.txt --> virtual disk (FAT32, firmware)|
 +-----------------------------------------------------------------------+
```

The app and the runner are two processes. The runner needs root (it runs
through `su`). It maps the iPod memory at the iPod addresses, near address
0. Android does not permit this for a normal app (`mmap_min_addr`).

## The app

The app is plain Java for Android 4.2, without libraries
(`android-app/app/src/main/java/org/retailos/`).

| Class | Task |
|---|---|
| `StartActivity` | The start screen with "Build database" and "Start iPod". The activity is also a home screen: as the default home screen it starts the iPod when the Y1 starts. |
| `IpswSetup` | Reads the IPSW (a ZIP file). Writes the firmware partition (`firmware.bin`) and the retailOS image (`osos.bin`). |
| `MediaLibrary` | Scans `/sdcard/Music`, `/sdcard/Podcasts`, `/sdcard/Audiobooks`. Reads the tags (`MediaMetadataRetriever`). Writes the database and the manifest. |
| `ITunesDb` | Writes the iTunesDB: the songs, the playlists, the podcasts. |
| `ArtworkDb`, `CoverArt` | Write the album covers: the ArtworkDB and the image files. |
| `AudioInfo` | Reads the sample rate from the audio file headers (MP3, MP4, WAV). |
| `NativeActivity` | Starts the runner. Connects the Y1 to the runner: keys, battery, screen, sound, brightness. |
| `WheelMenu` | The list of the start screen, for the click wheel. |

### The firmware file

An IPSW is a ZIP file with a `Firmware-*` file: the iPod firmware
partition. A directory in the IPSW ("!ATA" entries) lists the images:
`osos` (retailOS), `rsrc` (a FAT12 volume with fonts and images), and
`aupd` (the flash updater). The app writes the partition to `firmware.bin`
and the `osos` image to `osos.bin`.

The 5.5G file (`iPod_25.1.3.ipsw`) and the 5G file (`iPod_20.1.3.ipsw`)
contain the same `osos` and `rsrc` bytes. Only the layout differs. The 5.5G
file has a 2048-byte header before each image (one sector of the 5.5G
disk). The 5G file has a 512-byte header. retailOS reads an image at its
directory offset + 0x200 + its entry field. Therefore the app adds 0x600 to
each directory offset of a 5.5G file (`IpswSetup.layout()`).

### The music database

The iPod finds its music through `iPod_Control/iTunes/iTunesDB`, as iTunes
writes the file. The app writes the same file:

- a track record (`mhit`) for each file: tags, length, sample rate, media
  type (song, podcast, audiobook), and the path in the iTunes form
  (`:iPod_Control:Music:F00:R00000.MP3`),
- the master playlist, and the podcast playlists (grouped by show),
- the album covers: `iPod_Control/Artwork/ArtworkDB` with the files
  `F1028_1.ithmb` (100x100) and `F1029_1.ithmb` (200x200), RGB565. A cover
  links to a track through the track database ID.

The audio files stay where they are. The manifest
(`/sdcard/retailos/manifest.txt`) maps each iPod path to the real file. The
runner shows the files to the iPod under the iPod paths.

### The Y1 as an iPod

`NativeActivity` starts the runner with `su` and connects these channels:

| Channel | From | To | Content |
|---|---|---|---|
| stdin | app | runner | One byte per key: wheel steps, button press and release, the hold switch, the battery state |
| stdout | runner | app | The audio: PCM, 16 bit, stereo, 44100 Hz. An `AudioTrack` plays it. |
| `frames.fifo` | runner | app | Each screen frame (320x240, RGB565). Events: the volume, the backlight level. |
| SIGTERM | app | runner | Hibernate: save the state and exit. |

The Y1 keys become the iPod controls. The wheel turns the click wheel. The
center button is SELECT. BACK is MENU. A held key is a held button (Android
repeats a held key; the app sends one press and one release). BACK and PLAY
together toggle the hold switch.

The app copies the Y1 battery level and charger state to the iPod power
chip. The app applies the iPod headphone volume to the `AudioTrack`. The app
applies the iPod backlight level to the screen brightness. When the iPod
backlight is off, the app dims the frame nearly to black.

The `AudioTrack` buffer is 0.5 s, because the runner sends the samples in
bursts (a screen update stops them for about 0.1 s). If the buffer runs
empty during playback, the app makes the buffer longer, up to 1 s.

## The native runner

The runner is a C program (`native-runner/`). Build it with the Android NDK
r10e. The runner loads `osos.bin` and starts it at the reset vector, as the
iPod boot ROM does.

### Memory

The runner maps the iPod memory at the iPod addresses in its own process:

- the 64 MB SDRAM at 0x10000000, and after the firmware remap also at 0x0
  and at the alias addresses (shared memory, one copy),
- the 128 KB IRAM at 0x40000000,
- the boot ROM state: the NOR flash with the hardware revision, and the
  SysInfo block in IRAM that the firmware reads at the start.

Therefore a load or a store of the firmware to its memory is a plain host
access, at full speed. The device registers (0x60000000, 0x70000000, the
IDE controller, the BCM2722) are not mapped. The kernel cannot map page 0
(the first 4 KB).

### Trap and emulate

What the processor cannot do for the firmware arrives in the runner as a
signal:

| Signal | Cause | Handled by |
|---|---|---|
| SIGILL | A privileged instruction (MRS, MSR, exception return). The loader replaces each one with a trap word when it loads the image. | The virtual CPU (`vcpu.c`): mode, interrupt masks, banked registers |
| SIGSEGV | An access to a device register, or code in page 0 | The device models (`dev_*.c`) |
| SIGALRM | Each 1 ms | The timers: an interrupt can come while the firmware runs code that does not trap |

A signal costs 20 to 40 µs on this kernel. Therefore after a trap the
handler does not return at once. The handler runs the next instructions
itself while more traps follow (run-ahead), with a template JIT (`jit.c`)
or an interpreter (`interp.c`). An interrupt handler with its device
accesses then costs one signal, not hundreds. Code that runs often in the
handler (the firmware code ranges) keeps its JIT translation.

### Paravirtual interrupt masking

The firmware masks the interrupts through two small functions, thousands of
times per second. Each call had two privileged instructions, therefore two
traps. The loader sends both functions to small stubs in host memory
(`pv.c`). The stubs change a shadow copy of the interrupt masks without a
trap. Only a change that lets a waiting interrupt in still traps.

### Interrupts, the second core, the time

- Interrupts are real ARM exceptions on the virtual CPU. The runner
  delivers an IRQ or FIQ only when the virtual CPSR permits it. The
  firmware vectors, dispatcher and task switch run unchanged.
- The COP (the second core) decodes the audio. The runner runs the COP on
  the same thread: in slices through the JIT while the CPU is awake, and
  natively while the CPU sleeps.
- Guest time is host time without the cost of the emulation (the handler
  time and the kernel cost of each trap do not count). When the iPod
  waits, the runner moves guest time forward to the next timer instead of
  a sleep. Therefore guest time keeps pace with real time.

### Device models

The device models are ports of the QEMU models of the iPod video
(`qemu-david`), with additions from Rockbox and from traces of the firmware:

| Model | File | Emulates |
|---|---|---|
| SoC | `dev_soc.c` | Interrupt controller, timers, mailbox between the cores, processor control (sleep, wake), cache control, memory remap, clocks, click wheel, clicker, serial port |
| GPIO | `dev_gpio.c` | Ports A-L and their interrupts: the hold switch, the power pins (charger, FireWire power) |
| Backlight | `dev_backlight.c` | The LED driver (on, off, 32 levels) |
| DMA, I2S | `dev_dma.c`, `dev_i2s.c` | The audio path to the DAC: the samples go to stdout |
| I2C | `dev_i2c.c` | The PCF50605 power chip (battery ADC, charger) and the WM8758 codec (headphone volume) |
| IDE | `dev_ide.c` | The hard disk: reads from the virtual disk, writes to an overlay |
| BCM2722 | `dev_bcm.c` | The video chip: the firmware VideoCore calls, surfaces, the frame on the screen |
| Frame stream | `frameout.c` | The frames and the events to the app |

### The virtual disk

The iPod sees a hard disk with two partitions (`vdisk.c`):

1. The firmware partition: `firmware.bin` from the IPSW.
2. A FAT32 volume. The runner makes the volume in memory from the folder
   `/sdcard/retailos/disk`, with the iPod own files (the database, the
   covers, notes, contacts). The manifest adds the music files at their
   iPod paths. The file data comes from the real files. Nothing is copied.

The disk has the size and the free space of the Y1 storage. The firmware
writes stay in an overlay: the audio files are read-only for the iPod.

### Hibernate

When the app goes to the background (HOME), the app sends SIGTERM. The
runner then saves its full state (`snap.bin`: CPU, COP, IRAM, the devices,
the disk writes, the guest time, and the SDRAM in `guest-dram.bin`) and
exits. The next start resumes from the snapshot in about 2 s, as a sleeping
iPod wakes up: the language, the menu and the song continue. A new build of
the runner or a new database makes a cold start.

The firmware does not save its settings on the disk in this emulation.
Therefore the settings stay only in the saved state.

## What runs natively and what is emulated

| Part | How it runs |
|---|---|
| The firmware ordinary instructions (both cores): the menus, the database, the MP3/AAC decoder, the drawing | Natively, on the Y1 processor |
| Loads and stores to the iPod SDRAM and IRAM | Natively (the memory is mapped at the iPod addresses) |
| The firmware interrupt masking | Natively in paravirtual stubs, except a change that lets an interrupt in |
| Privileged instructions (MRS, MSR, exception return) | Emulated on the virtual CPU, after a trap |
| Code just after a trap, code in page 0 | Emulated: JIT or interpreter in the signal handler |
| Interrupts, exceptions, task switches | The firmware own code, started by the runner on the virtual CPU |
| The second core (COP) | Natively while the CPU sleeps, otherwise through the JIT in slices |
| Device registers of the PP5022, BCM2722, I2C chips, disk | Emulated: device models |
| The boot ROM | Not run. The runner sets the state that the boot ROM leaves (NOR, SysInfo) and starts retailOS at its reset vector. |
| The LCD, the DAC, the backlight, the battery, the buttons | Replaced by the Y1: SurfaceView, AudioTrack, screen brightness, battery state, keys |
| The hard disk | Replaced by a virtual disk from a folder and the Y1 audio files |
| iTunes | Replaced by the app "Build database" |
| USB, FireWire data, video out, the dock | Not emulated (only the FireWire power pin, for the charger icon) |
