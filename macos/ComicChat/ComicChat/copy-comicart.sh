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

set -eu

SRC_COMICART="$SRCROOT/../../v2.5-beta-1-modern/comicart"
SRC_ARTPACK1="$SRCROOT/../../v2.5-beta-1-modern/artpack1"
DEST="$BUILT_PRODUCTS_DIR/$UNLOCALIZED_RESOURCES_FOLDER_PATH/comicart"

mkdir -p "$DEST"

rsync -a --delete "$SRC_COMICART"/ "$DEST"/

ARTPACK1_FILES="kevin.avb kwensa.avb maynard.avb rebecca.avb sage.avb scotty.avb den.bgb volcano.bgb"
for f in $ARTPACK1_FILES; do
    rsync -a "$SRC_ARTPACK1/$f" "$DEST/$f"
done

echo "copy-comicart.sh: $(ls "$DEST" | wc -l | tr -d ' ') files in $DEST"
