#include <stdlib.h>
#include <stdio.h>
#include <xenstore.h>

// from service.c
int service_main(void);

int qubes_setup_network() {
	struct xs_handle *xs;
	char *interface_name = "14";
	char *qubes_ip;
	char *qubes_netmask;
	char *qubes_gateway;
	int ret = 1;
	char cmdline[255];

	xs = xs_domain_open();
	if (!xs) {
		fprintf(stderr, "Failed to open xenstore connection\n");
		goto cleanup;
	}

	qubes_ip = xs_read(xs, XBT_NULL, "qubes-ip", NULL);
	if (!qubes_ip) {
		fprintf(stderr, "Failed to get qubes_ip\n");
		goto cleanup;
	}
	qubes_netmask = xs_read(xs, XBT_NULL, "qubes-netmask", NULL);
	if (!qubes_netmask) {
		fprintf(stderr, "Failed to get qubes_netmask\n");
		goto cleanup;
	}
	qubes_gateway = xs_read(xs, XBT_NULL, "qubes-gateway", NULL);
	if (!qubes_gateway) {
		fprintf(stderr, "Failed to get qubes_gateway\n");
		goto cleanup;
	}

	_snprintf(cmdline, sizeof(cmdline), "netsh interface ipv4 set address \"%s\" static %s %s %s 1",
			interface_name, qubes_ip, qubes_netmask, qubes_gateway);
	if (system(cmdline)!=0) {
		fprintf(stderr, "Failed to set IP address by calling: %s\n", cmdline);
		goto cleanup;
	}

	_snprintf(cmdline, sizeof(cmdline), "netsh interface ipv4 set dnsservers \"%s\" static %s register=none validate=no",
			interface_name, qubes_gateway);
	if (system(cmdline)!=0) {
		fprintf(stderr, "Failed to set DNS address by calling: %s\n", cmdline);
		goto cleanup;
	}

	ret = 0;

cleanup:
	if (qubes_ip)
		free(qubes_ip);
	if (qubes_netmask)
		free(qubes_netmask);
	if (qubes_gateway)
		free(qubes_gateway);

	if (xs)
		xs_daemon_close(xs);

	return ret;
}

int main(int argc, char **argv) {
	if (argc >= 2 && 0==strcmp(argv[1], "-service")) {
		return service_main();
	} else {
		return qubes_setup_network();
	}
}
