ARCH := arm64
ARM_CPU := cortex-a53
TARGET := maestro9610

WITH_KERNEL_VM := 0
WITH_LINKER_GC := 1

GLOBAL_DEFINES += \
	SYSPARAM_ALLOW_WRITE=1 \
	WITH_LIB_CONSOLE=1

MODULES += \
	app/exynos_boot \
	app/shell

MODULE_DEPS += \
	lib/gpt \
	lib/font \
	lib/logo \
	lib/libavb \
	lib/cksum \
	lib/fdt \
	lib/ufdt \
	dev/scsi \
	dev/rpmb \
	dev/boot \
	dev/dpu \
	dev/adc
