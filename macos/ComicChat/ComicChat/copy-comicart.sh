#!/bin/sh
# Plan 4a Task 10: bundles the comicart resources the engine needs at
# runtime (avatar .avb / backdrop .bgb files) into the built app.
#
# Sources are the checked-out v2.5-beta-1-modern tree (READ-ONLY — this
# script only ever rsyncs FROM it, never writes into it):
#   - comicart/            all 32 files (the full stock art pack)
#   - artpack1/             6 unique avatars (kevin, kwensa, maynard,
#                            rebecca, sage, scotty) + 2 unique backdrops
#                            (den, volcano) not already present in comicart/
#
# Destination: $BUILT_PRODUCTS_DIR/$UNLOCALIZED_RESOURCES_FOLDER_PATH/comicart
# i.e. ComicChat.app/Contents/Resources/comicart — resolved by
# AppState.connect() via Bundle.main.resourceURL at runtime.
#
# Plan 4b Task 3: also bundles the emotion wheel's 8 face icon BMPs
# (res/fc_{hap,coy,bor,sca,sad,ang,sho,laf}_l.bmp — bodycam.cpp:49-59's
# lg_icons order) into Resources/wheel/, resolved by BodyCamView via
# Bundle.main.url(forResource:withExtension:subdirectory:).

set -eu

SRC_COMICART="$SRCROOT/../../v2.5-beta-1-modern/comicart"
SRC_ARTPACK1="$SRCROOT/../../v2.5-beta-1-modern/artpack1"
SRC_RES="$SRCROOT/../../v2.5-beta-1-modern/res"
DEST="$BUILT_PRODUCTS_DIR/$UNLOCALIZED_RESOURCES_FOLDER_PATH/comicart"
DEST_WHEEL="$BUILT_PRODUCTS_DIR/$UNLOCALIZED_RESOURCES_FOLDER_PATH/wheel"

mkdir -p "$DEST"

rsync -a --delete "$SRC_COMICART"/ "$DEST"/

ARTPACK1_FILES="kevin.avb kwensa.avb maynard.avb rebecca.avb sage.avb scotty.avb den.bgb volcano.bgb"
for f in $ARTPACK1_FILES; do
    rsync -a "$SRC_ARTPACK1/$f" "$DEST/$f"
done

echo "copy-comicart.sh: $(ls "$DEST" | wc -l | tr -d ' ') files in $DEST"

mkdir -p "$DEST_WHEEL"

WHEEL_ICON_FILES="fc_hap_l.bmp fc_coy_l.bmp fc_bor_l.bmp fc_sca_l.bmp fc_sad_l.bmp fc_ang_l.bmp fc_sho_l.bmp fc_laf_l.bmp"
for f in $WHEEL_ICON_FILES; do
    rsync -a "$SRC_RES/$f" "$DEST_WHEEL/$f"
done

echo "copy-comicart.sh: $(ls "$DEST_WHEEL" | wc -l | tr -d ' ') files in $DEST_WHEEL"
