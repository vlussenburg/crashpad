#!/bin/bash

NDK=$HOME/Android/Sdk/ndk/20.1.5948944
OUT=out/Android/ndk20b/

function gen()
{
    ARCH=$1

    if [[ $ARCH == x64 ]]; then
        DIR=x86_64
        API=21
    elif [[ $ARCH == arm64 ]]; then
        DIR=arm64-v8a
        API=21
    elif [[ $ARCH == arm ]]; then
        DIR=armeabi-v7a
        API=19
    else
        echo "unsupported architecture"
        exit 1
    fi

    for CFG in Debug Release; do
        echo "Generating build file for $ARCH API_LEVEL=$API to $OUT/$CFG/$DIR"
        python2 build/gyp_crashpad_android.py --ndk $NDK --arch $ARCH --api-level $API --generator-output $OUT/$CFG/$DIR -G config=$CFG
    done
}


if [[ $# == 1 ]]; then 
    gen $1
else
    for arch in x64 arm64 arm; do
        gen $arch
    done
fi
