#!/bin/bash
# Build android-app/build/retailos-native.apk with the current native runner.
# SDK: the API-17 android.jar and build tools (SDK=..., BT=... to change).
# The APK is signed with the AOSP platform keys in this directory.
set -e
cd "$(dirname "$0")"
SDK=${SDK:-$HOME/Android/Sdk}
BT=${BT:-$SDK/build-tools/36.0.0}
JAR=$SDK/platforms/android-17/android.jar
NDK=${NDK:-$HOME/android-ndk-r10e}
STRIP=${STRIP:-$NDK/toolchains/arm-linux-androideabi-4.9/prebuilt/linux-x86_64/bin/arm-linux-androideabi-strip}
B=android-app/build
make -C native-runner arm NDK="$NDK"
# The APK gets only the runner: no Apple file (the app takes the firmware
# from the user's IPSW), and no stray file from the assets folder.
rm -rf $B/assets && mkdir -p $B/assets
# stripped: without debug info, the APK has no build paths of this computer
# (native-runner/build/runner keeps them, for the tools)
"$STRIP" -o $B/assets/runner-native native-runner/build/runner
rm -rf $B/classes && mkdir -p $B/classes
# against Android 17's own java.* classes: a newer API (such as
# StandardCharsets) fails here, not on the Y1
javac -source 8 -target 8 -bootclasspath $JAR -Xlint:-options -d $B/classes android-app/app/src/main/java/org/retailos/*.java
rm -f $B/classes.dex
$BT/d8 --min-api 17 --lib $JAR --output $B $(find $B/classes -name '*.class')
$BT/aapt2 link --manifest android-app/app/src/main/AndroidManifest.xml -I $JAR \
    -A $B/assets --min-sdk-version 17 --target-sdk-version 17 -o $B/app-unsigned.apk
(cd $B && zip -q app-unsigned.apk classes.dex)
$BT/zipalign -f 4 $B/app-unsigned.apk $B/app-aligned.apk
$BT/apksigner sign --key platform.pk8 --cert platform.x509.pem --out $B/retailos-native.apk $B/app-aligned.apk
echo "built $B/retailos-native.apk"
