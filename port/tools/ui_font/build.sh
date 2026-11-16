#!/bin/sh
# Regenerates port/src/plat/ui_font_data.h. FONT_DIR holds the Google Fonts files (OFL 1.1) named as below.
set -e
D=${FONT_DIR:-.}
python3 "$(dirname "$0")/make_ui_font.py" "$(dirname "$0")/../../src/plat/ui_font_data.h" \
  "Outfit:$D/Outfit-Medium.ttf:13:10:::draw=outfit/2/0/0" "OutfitHead:$D/Outfit-Bold.ttf:15:11:::draw=outfit/0/2/2" \
  "OutfitXL:$D/Outfit-Bold.ttf:36:28::0123456789:lgdraw=18/26/5/4/3" "OutfitLG:$D/Outfit-Bold.ttf:20:15:::lgdraw=11/14/3/2/2" "OutfitSM:$D/Outfit-SemiBold.ttf:12:9:::draw=outfit/0/0/0" \
  "Barlow:$D/BarlowCondensed-SemiBold.ttf:14:10:::draw=barlow/0/2/0" "BarlowHead:$D/BarlowCondensed-Bold.ttf:16:11:::draw=barlow/2/2/2" \
  "BarlowXL:$D/BarlowCondensed-Bold.ttf:38:28::0123456789:lgdraw=14/26/5/4/2//slant" "BarlowLG:$D/BarlowCondensed-Bold.ttf:22:15:::lgdraw=9/16/3/2/1/barlow" "BarlowSM:$D/BarlowCondensed-SemiBold.ttf:13:9:::draw=barlow/2/0/0" \
  "Glass:$D/Inter-SemiBold.otf:12:10:::draw=glass/0/0/0" "GlassHead:$D/Inter-Bold.otf:12:10:::draw=glass/0/0/0" \
  "GlassXL:$D/Inter-Bold.otf:34:28::0123456789:lgdraw=16/24/5/4/2" "GlassLG:$D/Inter-Bold.otf:19:15:::lgdraw=10/14/3/2/1" "GlassSM:$D/Inter-Bold.otf:11:9:::draw=glass/0/0/0" \
  "Classic:$D/Classic.ttf:cap8:10:::outline+pix8" "ClassicHead:$D/Classic.ttf:cap9:11:::outline+pix10" \
  "ClassicXL:$D/Classic.ttf:cap21:28::0123456789:outline+lgdraw=15/22/5/4/2" "ClassicLG:$D/Classic.ttf:cap12:15:::outline+lgdraw=9/12/3/2/1" "ClassicSM:$D/Classic.ttf:cap8:9:::outline+pix8"
