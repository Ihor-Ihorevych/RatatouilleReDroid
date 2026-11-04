#!/usr/bin/env bash
# Build the Ratatouille Android APK (arm64, debug-signed) without Gradle, from a Git Bash on Windows.
#   android/build-apk.sh              build/android/ratatouille.apk
#   android/build-apk.sh --install    ... and install it on the connected phone (adb)
#   android/build-apk.sh --recomp     run xbe_recomp first (tools-build/build/xbe_recomp.exe) to refresh generated/game
# The APK holds code translated from the game: for your own phone only, never publish or share it.
# Needs: JDK 17+, Android SDK (platform 35, build-tools, NDK 28.2.13676358, CMake 4.1.2), a built tools-build for --recomp.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

ANDROID_HOME="${ANDROID_HOME:-${LOCALAPPDATA:-$HOME/AppData/Local}/Android/Sdk}"
ANDROID_HOME="$(cygpath -u "$ANDROID_HOME")"
NDK="$ANDROID_HOME/ndk/28.2.13676358"
CMAKE="$ANDROID_HOME/cmake/4.1.2/bin"
BT="$(ls -d "$ANDROID_HOME"/build-tools/* | sort -V | tail -1)"
AJ="$ANDROID_HOME/platforms/android-35/android.jar"
ADB="$ANDROID_HOME/platform-tools/adb.exe"
OUT="$ROOT/build/android"
APK_DIR="$OUT/apk"
INSTALL=0
RECOMP=0
SELFPACK=0  # --selfpack: also ratatouille-selfpack.apk, which carries game_files and unpacks them on the first start
for a in "$@"; do
	case "$a" in
		--selfpack) SELFPACK=1 ;;
		--install) INSTALL=1 ;;
		--recomp) RECOMP=1 ;;
		*) echo "unknown argument: $a" >&2; exit 1 ;;
	esac
done

if [ "$RECOMP" = 1 ] || [ ! -f generated/game/x86_func_mapping.cpp ]; then
	[ -f assets/default.xbe ] || { echo "assets/default.xbe is missing: extract it from the disc image" >&2; exit 1; }
	[ -x tools-build/build/xbe_recomp.exe ] || { echo "tools-build/build/xbe_recomp.exe is missing (cmake --build tools-build/build)" >&2; exit 1; }
	echo "[1/5] recompiling default.xbe"
	tools-build/build/xbe_recomp.exe config/game.toml --xdk sdk/xdk | tail -3
fi

echo "[2/5] native code"
export PATH="$CMAKE:$PATH"
[ -f "$OUT/build.ninja" ] || cmake -S android -B "$OUT" -G Ninja \
	-DCMAKE_TOOLCHAIN_FILE="$(cygpath -m "$NDK")/build/cmake/android.toolchain.cmake" -DANDROID_ABI=arm64-v8a \
	-DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_static -DCMAKE_BUILD_TYPE=RelWithDebInfo \
	-DCMAKE_MAKE_PROGRAM="$(cygpath -m "$CMAKE")/ninja.exe"
cmake --build "$OUT" --target main

echo "[3/5] java"
SDL_SRC="$(grep -i -m1 '^sdl3_SOURCE_DIR' "$OUT/CMakeCache.txt" | sed 's/.*=//' || true)"
[ -n "$SDL_SRC" ] || SDL_SRC="$OUT/_deps/sdl3-src"
SDL_SRC="$(cygpath -u "$SDL_SRC")"
rm -rf "$APK_DIR"
mkdir -p "$APK_DIR/classes" "$APK_DIR/lib/arm64-v8a"
javac -encoding UTF-8 -Xlint:-options -source 8 -target 8 -cp "$(cygpath -m "$AJ")" -d "$(cygpath -m "$APK_DIR/classes")" \
	"$SDL_SRC"/android-project/app/src/main/java/org/libsdl/app/*.java android/java/io/github/ratatouille/*.java
(cd "$APK_DIR" && find classes -name "*.class" > classes.list && "$BT/d8.bat" --lib "$(cygpath -m "$AJ")" --output . @classes.list)

echo "[4/5] packaging"
STRIP="$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-strip.exe"
"$STRIP" --strip-unneeded -o "$APK_DIR/lib/arm64-v8a/libmain.so" "$OUT/libmain.so"
cp "$OUT/libSDL3.so" "$APK_DIR/lib/arm64-v8a/" 2>/dev/null || cp "$(find "$OUT" -name libSDL3.so | head -1)" "$APK_DIR/lib/arm64-v8a/"
[ -f android/debug.keystore ] || keytool -genkeypair -keystore android/debug.keystore -storepass android -keypass android \
	-alias debug -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=Ratatouille Debug"
"$BT/aapt2.exe" compile --dir android/res -o "$APK_DIR/res.zip"
"$BT/aapt2.exe" link -o "$APK_DIR/base.apk" --manifest android/AndroidManifest.xml -I "$(cygpath -m "$AJ")" \
	-R "$APK_DIR/res.zip" --min-sdk-version 29 --target-sdk-version 35
(cd "$APK_DIR" && cp base.apk unsigned.apk && jar uf unsigned.apk classes.dex lib && "$BT/zipalign.exe" -f -p 4 unsigned.apk aligned.apk)

if [ "$SELFPACK" = 1 ]; then
	# the game rides in the APK as stored (uncompressed) assets, listed in assets/game.list for SetupActivity to unpack
	[ -f game_files/default.xbe ] || { echo "game_files/default.xbe is missing: extract the disc image there" >&2; exit 1; }
	echo "      selfpack: adding game_files"
	PACK="$OUT/selfpack"
	rm -rf "$PACK"
	mkdir -p "$PACK/assets"
	cp -r game_files "$PACK/assets/game"
	(cd game_files && find . -type f -printf '%s\t%P\n') > "$PACK/assets/game.list"
	cp "$APK_DIR/unsigned.apk" "$APK_DIR/unsigned-selfpack.apk"
	(cd "$PACK" && 7z a -tzip -mx0 -bso0 -bsp0 "$(cygpath -m "$APK_DIR/unsigned-selfpack.apk")" assets)
	(cd "$APK_DIR" && "$BT/zipalign.exe" -f -p 4 unsigned-selfpack.apk aligned-selfpack.apk)
fi

echo "[5/5] signing"
"$BT/apksigner.bat" sign --ks android/debug.keystore --ks-pass pass:android --key-pass pass:android \
	--out "$OUT/ratatouille.apk" "$APK_DIR/aligned.apk"
ls -l "$OUT/ratatouille.apk"
APK="$OUT/ratatouille.apk"
if [ "$SELFPACK" = 1 ]; then
	"$BT/apksigner.bat" sign --ks android/debug.keystore --ks-pass pass:android --key-pass pass:android \
		--out "$OUT/ratatouille-selfpack.apk" "$APK_DIR/aligned-selfpack.apk"
	ls -l "$OUT/ratatouille-selfpack.apk"
	APK="$OUT/ratatouille-selfpack.apk"
fi
if [ "$INSTALL" = 1 ]; then
	"$ADB" install -r "$(cygpath -m "$APK")"
fi
