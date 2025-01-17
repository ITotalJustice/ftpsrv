#!/bin/sh

# Get the current version number
VERSION=$(cat version.txt)

# builds a preset
build_preset() {
    echo Configuring $1 ...
    cmake --preset $1
    echo Building $1 ...
    cmake --build --preset $1
}

build_preset core
build_preset nds
build_preset 3ds
build_preset wii
build_preset switch

rm -rf out

# --- NDS --- #
mkdir -p out/nds/config/ftpsrv
cp assets/config.ini.template out/nds/config/ftpsrv/

mkdir -p out/nds
cp build/nds/*.nds out/nds/ftpsrv.nds
cd out/nds
zip -r9 ../nds_v${VERSION}.zip ftpsrv.nds config
cd ../..

# --- 3DS --- #
mkdir -p out/3ds/config/ftpsrv
cp assets/config.ini.template out/3ds/config/ftpsrv/

mkdir -p out/3ds
cp build/3ds/*.3dsx out/3ds/ftpsrv.3dsx
cd out/3ds
zip -r9 ../3ds_v${VERSION}.zip ftpsrv.3dsx config
cd ../..

# --- WII --- #
mkdir -p out/wii/config/ftpsrv
cp assets/config.ini.template out/wii/config/ftpsrv/

mkdir -p out/wii
cp -r build/wii/apps out/wii/
cd out/wii
zip -r9 ../wii_v${VERSION}.zip apps config
cd ../..

# --- SWITCH --- #
mkdir -p out/switch/config/ftpsrv
cp assets/config.ini.template out/switch/config/ftpsrv/

mkdir -p out/switch/switch
cp assets/config.ini.template out/switch/config/ftpsrv/
cp -r build/switch/*.nro out/switch/switch/ftpsrv.nro
cd out/switch
zip -r9 ../switch_application_v${VERSION}.zip switch config
cd ../..

mkdir -p out/switch/atmosphere/contents/
cp -r build/switch/420000000000011B out/switch/atmosphere/contents/
cd out/switch
zip -r9 ../switch_sysmod_v${VERSION}.zip atmosphere config
cd ../..
