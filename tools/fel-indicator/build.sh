#!/bin/sh
# Builds fel-indicator.apk with the Android SDK tools only (no Gradle). Signs with the debug key
# unless KEYSTORE / KEY_ALIAS / KEY_PASS are set. Output: video/fel-indicator.apk
set -e
cd "$(dirname "$0")"
SDK=${ANDROID_HOME:-$HOME/Android/Sdk}
BT=$SDK/build-tools/${BUILD_TOOLS:-35.0.0}
JAR=$SDK/platforms/${PLATFORM:-android-35}/android.jar
OUT=build
rm -rf $OUT && mkdir -p $OUT/classes $OUT/dex
javac --release 8 -classpath "$JAR" -d $OUT/classes $(find src -name '*.java') 2>&1 | grep -v "^warning: \[options\]\|^1 warning" || true
"$BT/d8" --release --min-api 26 --lib "$JAR" --output $OUT/dex $(find $OUT/classes -name '*.class')
"$BT/aapt2" link -o $OUT/unsigned.apk -I "$JAR" --manifest AndroidManifest.xml
(cd $OUT/dex && zip -q ../unsigned.apk classes.dex)
"$BT/zipalign" -f 4 $OUT/unsigned.apk $OUT/aligned.apk
"$BT/apksigner" sign --v4-signing-enabled false --ks "${KEYSTORE:-$HOME/.android/debug.keystore}" --ks-key-alias "${KEY_ALIAS:-androiddebugkey}" \
  --ks-pass "pass:${KEY_PASS:-android}" --out ../../video/fel-indicator.apk $OUT/aligned.apk
echo "video/fel-indicator.apk"
