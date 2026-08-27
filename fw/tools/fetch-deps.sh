#!/usr/bin/env bash
# Fetch the vendor dependencies. They are gitignored on purpose: the Giris repo
# tracks hardware, and pinning ~500 MB of upstream firmware libraries into it (or
# into .gitmodules) is a decision to make deliberately, not a side effect of a
# first build. To convert to submodules later:
#   git submodule add https://github.com/ArteryTek/AT32F402_405_Firmware_Library.git fw/vendor/at32f402_405
#   git submodule add https://github.com/hathach/tinyusb.git                        fw/vendor/tinyusb
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p vendor

clone() {  # repo dir
  if [ -d "vendor/$2/.git" ]; then
    echo "vendor/$2 already present"
  else
    echo "cloning $1 -> vendor/$2"
    git clone --depth 1 "$1" "vendor/$2"
  fi
}

# Artery BSP. NOTE the licence: it is not BSD/MIT. Artery grants use, copying and
# distribution "for the purpose of design and development in conjunction with
# Artery microcontrollers" — fine for this product, but it is not a free licence
# and it does not travel to a non-Artery target.
clone https://github.com/ArteryTek/AT32F402_405_Firmware_Library.git at32f402_405

# TinyUSB (MIT). Supports this part upstream as OPT_MCU_AT32F402_405 via the dwc2
# port, including the AT32 quirk that forces TRDT = 9.
clone https://github.com/hathach/tinyusb.git tinyusb

echo "done."
