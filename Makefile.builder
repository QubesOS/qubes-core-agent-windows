ifeq ($(PACKAGE_SET),vm)
WIN_SOURCE_SUBDIRS= win
WIN_PREBUILD_CMD = set_version.bat
ifeq ($(BACKEND_VMM),xen)
WIN_BUILD_DEPS = core-vchan-xen vmm-xen-windows-pvdrivers
endif
ifeq ($(BACKEND_VMM),wni)
WIN_BUILD_DEPS = core-vchan-wni
endif
endif

