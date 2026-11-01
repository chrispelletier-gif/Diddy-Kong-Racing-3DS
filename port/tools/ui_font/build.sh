#!/bin/sh
# Regenerates port/src/plat/ui_font_data.h. FONT_DIR holds the Google Fonts files (OFL 1.1) named as below.
set -e
D=${FONT_DIR:-.}
python3 "$(dirname "$0")/make_ui_font.py" "$(dirname "$0")/../../src/plat/ui_font_data.h" \
  "Outfit:$D/Outfit[wght].ttf:13:10:Medium" "OutfitHead:$D/Outfit[wght].ttf:15:11:Bold" \
  "OutfitXL:$D/Outfit[wght].ttf:36:28:Bold:0123456789" "OutfitLG:$D/Outfit[wght].ttf:20:15:Bold" "OutfitSM:$D/Outfit[wght].ttf:12:9:SemiBold" \
  "Barlow:$D/BarlowCondensed-SemiBold.ttf:14:10" "BarlowHead:$D/BarlowCondensed-Bold.ttf:16:11" \
  "BarlowXL:$D/BarlowCondensed-Bold.ttf:38:28::0123456789" "BarlowLG:$D/BarlowCondensed-Bold.ttf:22:15" "BarlowSM:$D/BarlowCondensed-SemiBold.ttf:13:9" \
  "Fredoka:$D/Fredoka[wdth,wght].ttf:13:10:Medium" "FredokaHead:$D/Fredoka[wdth,wght].ttf:13:10:SemiBold" \
  "FredokaXL:$D/Fredoka[wdth,wght].ttf:34:28:Bold:0123456789" "FredokaLG:$D/Fredoka[wdth,wght].ttf:19:15:Bold" "FredokaSM:$D/Fredoka[wdth,wght].ttf:12:9:SemiBold"
