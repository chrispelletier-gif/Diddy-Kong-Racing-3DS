#!/bin/sh
# Regenerates port/src/plat/ui_font_data.h. FONT_DIR holds the Google Fonts files (OFL 1.1) named as below.
set -e
D=${FONT_DIR:-.}
python3 "$(dirname "$0")/make_ui_font.py" "$(dirname "$0")/../../src/plat/ui_font_data.h" \
  "Outfit:$D/Outfit-Medium.ttf:13:10" "OutfitHead:$D/Outfit-Bold.ttf:15:11" \
  "OutfitXL:$D/Outfit-Bold.ttf:36:28::0123456789" "OutfitLG:$D/Outfit-Bold.ttf:20:15" "OutfitSM:$D/Outfit-SemiBold.ttf:12:9" \
  "Barlow:$D/BarlowCondensed-SemiBold.ttf:14:10:::fix=14.25/Qgft" "BarlowHead:$D/BarlowCondensed-Bold.ttf:16:11:::fix=16/Qgft" \
  "BarlowXL:$D/BarlowCondensed-Bold.ttf:38:28::0123456789" "BarlowLG:$D/BarlowCondensed-Bold.ttf:22:15:::fix=22/Qgft" "BarlowSM:$D/BarlowCondensed-SemiBold.ttf:13:9" \
  "Glass:$D/Inter-SemiBold.otf:12:10:::fix=12/Q" "GlassHead:$D/Inter-Bold.otf:12:10:::fix=12/Q" \
  "GlassXL:$D/Inter-Bold.otf:34:28::0123456789" "GlassLG:$D/Inter-Bold.otf:19:15:::fix=19/Q" "GlassSM:$D/Inter-Bold.otf:11:9:::fix=11/Q" \
  "Classic:$D/Classic.ttf:cap8:10:::outline+pix8" "ClassicHead:$D/Classic.ttf:cap9:11:::outline+pix10" \
  "ClassicXL:$D/Classic.ttf:cap21:28::0123456789:outline" "ClassicLG:$D/Classic.ttf:cap12:15:::outline" "ClassicSM:$D/Classic.ttf:cap8:9:::outline+pix8"
