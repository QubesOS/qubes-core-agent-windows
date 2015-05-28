ifeq ($(PACKAGE_SET),vm)
WIN_COMPILER = msbuild
WIN_SOURCE_SUBDIRS = .
WIN_PACKAGE_CMD = true
WIN_BUILD_DEPS = windows-utils
WIN_PREBUILD_CMD = set_version.bat && powershell -executionpolicy bypass set_version.ps1
endif
