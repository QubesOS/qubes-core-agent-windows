help:
	@echo "make all      --- build all"
	@echo "make clean    --- clean all"

all:
	$(MAKE) all -C filecopy_common
	$(MAKE) all -C clipboard_copy
	$(MAKE) all -C clipboard_paste
	$(MAKE) all -C filecopy_agent
	$(MAKE) all -C filecopy_unpacker
	$(MAKE) all -C open_in_vm
	$(MAKE) all -C vm_file_editor
	$(MAKE) all -C wait_for_logon

clean:
	$(MAKE) clean -C filecopy_common
	$(MAKE) clean -C clipboard_copy
	$(MAKE) clean -C clipboard_paste
	$(MAKE) clean -C filecopy_agent
	$(MAKE) clean -C filecopy_unpacker
	$(MAKE) clean -C open_in_vm
	$(MAKE) clean -C vm_file_editor
	$(MAKE) clean -C wait_for_logon
