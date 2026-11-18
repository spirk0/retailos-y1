# retailOS on Y1

This is a shallow emulation layer for iPod 5.5 firmwares. Most of the code runs 
natively (since iPods and the Y1 share very similar processor architectures).
A few things still needed emulation, such as the display, the input, audio,... 

Because of this it is not running at 100% speed and has some input delay.

But it plays the music, podcasts and the audiobooks on the Y1, with
the iPod's menus, album covers, hold switch (MENU+PLAY) and backlight.

## What you need

- An Innioasis Y1
- The firmware file from Apple: `iPod_25.1.3.ipsw` (`iPod_20.1.3.ipsw` also works).
  You should be able to find it with Google, Apple still hosts it.
- The `rom.zip` from releases and [mtkclient](https://github.com/bkerler/mtkclient) to flash it

## Installation

1. Install MTKClient: https://github.com/bkerler/mtkclient
2. Download the latest firmware here: https://github.com/spirk0/retailos-y1/releases/latest 
3. Unpack the archive:
```
mkdir rom && cd rom
unzip ../rom.zip
```
4. Turn of the device, disconnect from the PC
5. Start the flashing process:
```
cd rom
python ../mtk.py w logo,uboot,bootimg,recovery,android,usrdata logo.bin,lk.bin,boot.img,recovery.img,system.img,userdata.img
```
6. Connect the device via USB
7. Unplug the device when the process has finished
8. Power on the device

## Using it
1. Find and download `iPod_25.1.3.ipsw`
2. Put it on your SD card at `/sdcard/retailos/iPod_25.1.3.ipsw`
3. Put your supported audio files (mp3, aac, wav, m4a, m4b, aiff) into these folders:
   - `/sdcard/Music/`: music
   - `/sdcard/Podcasts/`: podcasts (the album tag is the name of the show)
   - `/sdcard/Audiobooks/`: audiobooks
  
Select "Build database" again after you add or delete audio files.

## Manual install
### Build

```sh
./build-apk.sh
```

The result is `android-app/build/retailos-native.apk`.

### Install

1. Install the app:
   `adb install -r android-app/build/retailos-native.apk`
2. Copy the firmware file into the folder `/sdcard/retailos/`:
   `adb push iPod_25.1.3.ipsw /sdcard/retailos/`

## Resources

This builds on a lot of information scattered throughout the internet, mostly these:

Code:

- [qemu-ipod-classic](https://github.com/davidmonterocrespo24/qemu-ipod-classic):
  a QEMU fork with a model of the iPod video (PP5022 devices, BCM2722 video
  chip). The device models of the native runner are ports of these models
- [rockbox-y1](https://github.com/rockbox-y1/rockbox):
  the github build workflow and setup instructions for mtkclient

Hardware Information:

- [Rockbox](https://github.com/Rockbox/rockbox)
- [ipod-emulator](https://github.com/ndoo/ipod-emulator)
- [libgpod](https://github.com/gtkpod/libgpod)
- [iPodLinux wiki](http://www.ipodlinux.org/)

## Technical documentation

- [docs/architecture.md](docs/architecture.md): how it works, basic concepts, what runs natively and what is emulated
