ifeq ($(CONFIG_BUILD_ARM64_DT_OVERLAY),y)
dtbo-$(CONFIG_ARCH_DIWALI) += \
	wuyi-21125-diwail-overlay.dtbo \
	wuyi-21125-diwail-overlay-T0.dtbo\

wuyi-21125-diwail-overlay.dtbo-base := diwali.dtb diwalip.dtb
wuyi-21125-diwail-overlay-T0.dtbo-base := diwali.dtb diwalip.dtb
endif

always-y	:= $(dtb-y)
subdir-y	:= $(dts-dirs)
clean-files	:= *.dtb *.dtbo
