#!/bin/sh
#
# Build Xilinx QEMU.
# Build Xilinx QEMU device-trees.
# Edit the QEMU and HW_DTB varialbes in this script.
#
# Copy the config-zynqmp-pcie into .config
# Build the kernel
# Create your rootfs named zu-rootfs.cpio.gz
# Run this script
#

set -x

QEMU=${HOME}/work/xilinx/m-arch/build-qemu/aarch64-softmmu/qemu-system-aarch64
HW_DTB=${HOME}/dts/LATEST/SINGLE_ARCH/zcu102-arm.dtb

KERNEL=arch/arm64/boot/Image
DTB=arch/arm64/boot/dts/xilinx/zynqmp-zcu102-rev1.0.dtb

PCIE="-device xlnx-pcie-rp,bus=pcie.0,id=pcie.1,port=1,chassis=1 -device pci-bridge,addr=00.0,bus=pcie.1,id=pcie.2,chassis_nr=2 -netdev user,id=hostnet1,net=10.0.3.0/24 -device rtl8139,bus=pcie.2,addr=01.0,netdev=hostnet1,romfile="

RESET_APU="-device loader,addr=0xfd1a0104,data=0x8000000e,data-len=4"


${QEMU} -M arm-generic-fdt,linux=on -m 2G -hw-dtb ${HW_DTB}	\
	-dtb ${DTB}						\
	-serial mon:stdio					\
	-display none						\
	-kernel ${KERNEL}					\
	-append "root=/dev/sda1 rw rootwait init=/bin/sh console=ttyPS0 maxcpus=1 earlycon=cdns,mmio,0xFF000000,115200n8"						\
	-nic user -nic user -nic user -nic user			\
	-initrd zu-rootfs.cpio.gz				\
	${PCIE}							\
	${RESET_APU}						\
	$*

