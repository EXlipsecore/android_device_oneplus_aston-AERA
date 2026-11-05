# AERA Recovery device tree for OnePlus 12R (aston) / Ace 3 (astonc)

## Supported variants

| Device | Model | Codename | Region |
| --- | --- | --- | --- |
| OnePlus 12R | `CPH2585` | `aston` (`OP5D35L1`) | India |
| OnePlus 12R | `CPH2609` | `aston` (`OP5D35L1`) | Global / EU |
| OnePlus 12R | `CPH2611` | `aston` (`OP5D35L1`) | North America |
| OnePlus Ace 3 | `PJE110` | `astonc` (`OP5CF9L1`) | China |

## Features

- Display (1264x2780 @ 120 Hz BOE command-mode panel) and Synaptics S3910 touch
- Adreno 740 hardware-accelerated UI (`libEGL_adreno` + QTI Gralloc/Mapper 4.0)
- Qualcomm FBEv2 metadata & userdata decryption across Android 14–17+ (Stock & Custom)
- Virtual A/B (`vabc` / `snapuserd` & OEM `dm-snapshot`) full OTA flashing (`update_engine_sideload`)
- Automatic Oplus `my_company` / `my_preload` dynamic partition stub creation & repair for Stock OxygenOS / ColorOS booting
- F2FS filesystem compression (`fscompress`) on Format Data
- WCN6855 (`kiwi_v2`) Wi-Fi, ADB, MTP, USB-OTG, Fastbootd, Haptics, FRP wipe, ABL preservation, and Root Manager

## Build

```sh
mkdir -p ~/aera && cd ~/aera
repo init -u https://github.com/AERA-Recovery/android_manifest -b aera-16.0
repo sync -c -j$(nproc --all)

git clone https://github.com/EXlipsecore/android_device_oneplus_aston-AERA.git -b aera-16.0 device/oneplus/aston

source build/envsetup.sh
lunch twrp_aston-bp2a-eng
mka -j$(nproc --all) adbd recoveryimage
```
