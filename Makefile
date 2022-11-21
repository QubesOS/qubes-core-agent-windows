OUTDIR = bin/$(ARCH)
OUTDIR_ANY = bin/AnyCPU
CFLAGS += -Iinclude -DUNICODE -D_UNICODE -mwindows -D_WIN32_WINNT=0x0600 -std=c11 -fgnu89-inline
LDFLAGS += -Wl,--as-needed -lqubesdb-client -lwindows-utils -lshlwapi -lwtsapi32 -lwsock32 -liphlpapi -lsetupapi -lole32 -lcomctl32 -lrpcrt4 -luuid -lntdll -lws2_32 -Wl,--no-insert-timestamp

#TODO: drop ask-vm-and-run.exe
TOOLS = $(addprefix $(OUTDIR)/, advertise-tools.exe ask-vm-and-run.exe network-setup.exe prepare-volume.exe qrexec-agent.exe qrexec-client-vm.exe qrexec-wrapper.exe qrexec-stdio-forwarder.exe)
QREXEC_SERVICES = $(addprefix $(OUTDIR)/, clipboard-copy.exe clipboard-paste.exe file-receiver.exe file-sender.exe get-image-rgba.exe open-in-vm.exe open-url.exe set-gui-mode.exe vm-file-editor.exe wait-for-logon.exe window-icon-updater.exe)
RPC_FILES = $(addprefix $(OUTDIR_ANY)/qubes-rpc/,qubes.ClipboardCopy qubes.ClipboardPaste qubes.Filecopy qubes.GetAppMenus qubes.GetImageRGBA qubes.OpenInVM qubes.OpenURL qubes.SetDateTime qubes.SetGuiMode qubes.StartApp qubes.VMShell qubes.WaitForSession qubes.SuspendPostAll)
RPC_SCRIPTS = $(addprefix $(OUTDIR_ANY)/, get-appmenus.ps1 set-time.ps1 start-app.ps1 update-time.bat qvm-connect-tcp.bat)


all: $(OUTDIR) $(OUTDIR_ANY) $(OUTDIR_ANY)/qubes-rpc $(TOOLS) $(QREXEC_SERVICES) $(RPC_FILES) $(RPC_SCRIPTS) $(OUTDIR_ANY)/service-policy.exe $(OUTDIR_ANY)/service-policy.cfg $(OUTDIR)/relocate-dir.exe

$(OUTDIR) $(OUTDIR_ANY) $(OUTDIR_ANY)/qubes-rpc:
	mkdir -p $@

$(addprefix $(OUTDIR)/,qrexec-agent.exe qrexec-client-vm.exe qrexec-wrapper.exe): LDFLAGS+=-lvchan
$(OUTDIR)/advertise-tools.exe: LDFLAGS+=-lqubesdb-client

$(QREXEC_SERVICES): CFLAGS += -Isrc/qrexec-services/common


$(OUTDIR)/file-receiver.exe: src/qrexec-services/common/filecopy.c src/qrexec-services/common/filecopy-error.c
#$(OUTDIR)/file-receiver.exe: CFLAGS += -DNO_SHLWAPI_STRFCNS

$(OUTDIR)/file-sender.exe: src/qrexec-services/common/filecopy.c src/qrexec-services/common/filecopy-error.c $(OUTDIR)/file-sender.res
$(OUTDIR)/open-in-vm.exe: src/qrexec-services/common/filecopy.c src/qrexec-services/common/filecopy-error.c

# specific rules

$(OUTDIR)/file-sender.res: src/qrexec-services/file-sender/file-sender.rc
	$(WINDRES) -i $< -o $@ -O coff

$(OUTDIR_ANY)/service-policy.exe: $(wildcard src/service-policy/*.cs)
	mcs $^ -r:System.ServiceProcess.dll -out:$@

$(OUTDIR_ANY)/service-policy.cfg: src/service-policy/service-policy.cfg
	cp $^ $@

$(OUTDIR)/relocate-dir.exe: $(wildcard src/relocate-dir/*.c) src/relocate-dir/chkstk.S
	$(CC) $^ $(CFLAGS) -I$(DDK_PATH) -e NtProcessStartup -Wl,--subsystem,native -L $(OUTDIR) -lntdll -nostdlib -D__INTRINSIC_DEFINED__InterlockedAdd64 -municode -Wl,--no-insert-timestamp -o $@

$(RPC_FILES): $(OUTDIR_ANY)/qubes-rpc/%: src/qrexec-services/%
	cp $^ $@
	
$(RPC_SCRIPTS): $(OUTDIR_ANY)/%: src/qrexec-services/%
	cp $^ $@

# generic rules

.SECONDEXPANSION:
$(TOOLS): $(OUTDIR)/%.exe: $$(wildcard src/%/*.c)
	$(CC) $^ $(CFLAGS) $(LDFLAGS) -municode -o $@

$(QREXEC_SERVICES): $(OUTDIR)/%.exe: $$(wildcard src/qrexec-services/%/*.c)
	$(CC) $^ $(CFLAGS) $(LDFLAGS) -municode -o $@
