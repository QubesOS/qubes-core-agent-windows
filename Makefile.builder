ifeq ($(PACKAGE_SET),vm)
WIN_SOURCE_SUBDIRS= win
WIN_PREBUILD_CMD = set_version.bat
WIN_BUILD_DEPS = core-vchan-xen
ifeq ($(BACKEND_VMM),xen)
WIN_BUILD_DEPS += vmm-xen-windows-pvdrivers
endif
endif

