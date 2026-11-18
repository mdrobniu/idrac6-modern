#!/bin/bash
set -e
export ARCH=arm
export CROSS_COMPILE=/opt/oabi-tc/bin/arm-softfloat-linux-gnu-
cd /kmod/linux-2.6.23
# base config: versatile = ARM926 / ARMv5 (gives vermagic "ARMv5"), UP
make versatile_defconfig >/dev/null 2>&1
# force the options that make vermagic match "2.6.23.1 preempt mod_unload ARMv5" + enable TUN
set_opt(){ sed -i "/\b$1\b/d" .config; echo "$2" >> .config; }
set_opt CONFIG_PREEMPT        "CONFIG_PREEMPT=y"
sed -i 's/^CONFIG_PREEMPT_NONE=y/# CONFIG_PREEMPT_NONE is not set/' .config
sed -i 's/^CONFIG_PREEMPT_VOLUNTARY=y/# CONFIG_PREEMPT_VOLUNTARY is not set/' .config
set_opt CONFIG_MODULE_UNLOAD  "CONFIG_MODULE_UNLOAD=y"
set_opt CONFIG_MODVERSIONS    "# CONFIG_MODVERSIONS is not set"
set_opt CONFIG_TUN            "CONFIG_TUN=m"
yes "" | make oldconfig >/dev/null 2>&1
echo "=== effective config ==="; grep -E "CONFIG_PREEMPT=|CONFIG_MODULE_UNLOAD=|CONFIG_MODVERSIONS|CONFIG_TUN=|CONFIG_CPU_32v5=|CONFIG_SMP=" .config
echo "=== modules_prepare ==="; make modules_prepare >/dev/null 2>build_prep.log || { tail -20 build_prep.log; exit 1; }
echo "=== build drivers/net modules (incl tun.ko) ==="
make SUBDIRS=drivers/net modules 2>build_mod.log || { echo "BUILD FAIL"; tail -25 build_mod.log; exit 1; }
ls -la drivers/net/tun.ko
echo "=== vermagic (must be: 2.6.23.1 preempt mod_unload ARMv5) ==="
/opt/oabi-tc/bin/arm-softfloat-linux-gnu-objdump -d -j .modinfo drivers/net/tun.ko >/dev/null 2>&1 || true
strings drivers/net/tun.ko | grep -iE "vermagic|^2\.6\.23"
cp drivers/net/tun.ko /kmod/tun.ko
echo "tun.ko copied to /kmod/tun.ko"
