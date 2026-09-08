# Planckian chromaticity data

`engine/scene/src/light_temperature_xy.inc` is derived from CIE 2019,
*Colour-matching functions of CIE 1931 standard colorimetric observer*,
International Commission on Illumination (CIE), Vienna, AT.

Source: https://doi.org/10.25039/CIE.DS.xvudnb9b

Dataset: https://files.cie.co.at/CIE_xyz_1931_2deg.csv

SHA-256: `fa663e3535a7e0763a745993a1f0a192eb0275ac46ad2d1befd7626841e713c1`

The original and derived data are licensed under Creative Commons
Attribution-ShareAlike 4.0 International:
https://creativecommons.org/licenses/by-sa/4.0/

Changes: integrate Planck spectra at 1 nm against the colour matching
functions, normalise to CIE xy, and tabulate temperatures from 1,000 to
40,000 kelvin in 100 kelvin increments. The table is data; the renderer code
and the generation script are separate works under the project's licence.

To reproduce, download the dataset above and run:

```text
python scripts/generate-light-temperature.py CIE_xyz_1931_2deg.csv
```

The generator verifies the source checksum. Neither network access nor
spectral integration occurs during configuration, building, or rendering.
