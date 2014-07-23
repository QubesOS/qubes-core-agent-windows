#include <stdlib.h>
#include <stdio.h>
#include <xenstore.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

// from service.c
int service_main(void);

int set_network_parameters(DWORD ip, DWORD netmask, DWORD gateway, PDWORD outInterfaceIndex) {
    PIP_ADAPTER_INFO	pAdapterInfo;
    PIP_ADAPTER_INFO	pAdapterInfoCurrent;
    PIP_ADDR_STRING		pAddrCurrent;
    MIB_IPINTERFACE_ROW	ipInterfaceRow;
    ULONG				ulOutBufLen;
    DWORD				NTEContext, NTEInstance;
    PMIB_IPFORWARDTABLE	pIpForwardTable = NULL;
    MIB_IPFORWARDROW	ipfRow;
    DWORD				dwSize = 0;
    DWORD				dwRetVal = 0;
    DWORD				i;

    /* clear this early, to not override dwForwardIfIndex */
    memset(&ipfRow, 0, sizeof(ipfRow));
    memset(&ipInterfaceRow, 0, sizeof(ipInterfaceRow));

    ulOutBufLen = 0;
    /* wait for adapters to initialize */
    while ((dwRetVal=GetAdaptersInfo( NULL, &ulOutBufLen)) == ERROR_NO_DATA) {
        fprintf(stderr, "GetAdaptersInfo call failed with %d, retrying\n", dwRetVal);
        Sleep(200);
    }

    if (dwRetVal != ERROR_BUFFER_OVERFLOW) {
        fprintf(stderr, "GetAdaptersInfo call failed with %d\n", dwRetVal);
        goto cleanup;
    }
    pAdapterInfo = (IP_ADAPTER_INFO *)malloc (ulOutBufLen);

    if ((dwRetVal = GetAdaptersInfo(pAdapterInfo, &ulOutBufLen)) != ERROR_SUCCESS) {
        printf("GetAdaptersInfo call(2) failed with %d\n", dwRetVal);
        goto cleanup;
    }

    /* set IP address */
    pAdapterInfoCurrent = pAdapterInfo;
    while (pAdapterInfoCurrent) {
        if (pAdapterInfoCurrent->Type == MIB_IF_TYPE_ETHERNET) {
            pAddrCurrent = &pAdapterInfoCurrent->IpAddressList;
            while (pAddrCurrent) {
                DeleteIPAddress(pAddrCurrent->Context);
                pAddrCurrent = pAddrCurrent->Next;
            }
            dwRetVal = AddIPAddress(ip, netmask, pAdapterInfoCurrent->Index, &NTEContext, &NTEInstance);
            if (dwRetVal != ERROR_SUCCESS) {
                fprintf(stderr, "AddIPAddress failed with %d\n", dwRetVal);
                goto cleanup;
            }
            ipfRow.dwForwardIfIndex = pAdapterInfoCurrent->Index;
            if (outInterfaceIndex)
                *outInterfaceIndex = ipfRow.dwForwardIfIndex;
        }
        pAdapterInfoCurrent = pAdapterInfoCurrent->Next;
    }

    /* set default gateway */
    dwRetVal = GetIpForwardTable(NULL, &dwSize, FALSE);
    if (dwRetVal == ERROR_INSUFFICIENT_BUFFER) {
        // Allocate the memory for the table
        pIpForwardTable = (PMIB_IPFORWARDTABLE) malloc(dwSize);
        if (pIpForwardTable == NULL) {
            printf("Unable to allocate memory for the IPFORWARDTALE\n");
            goto cleanup;
        }
        // Now get the table.
        dwRetVal = GetIpForwardTable(pIpForwardTable, &dwSize, FALSE);
    }

    if (dwRetVal != ERROR_SUCCESS) {
        fprintf(stderr, "getIpForwardTable failed with %d.\n", dwRetVal);
        goto cleanup;
    }

    // Search for the row in the table we want. The default gateway has a destination
    // of 0.0.0.0. Notice that we continue looking through the table, but copy only
    // one row. This is so that if there happen to be multiple default gateways, we can
    // be sure to delete them all.
    for (i = 0; i < pIpForwardTable->dwNumEntries; i++) {
        if (pIpForwardTable->table[i].dwForwardDest == 0) {
            // Delete the old default gateway entry.
            dwRetVal = DeleteIpForwardEntry(&(pIpForwardTable->table[i]));

            if (dwRetVal != ERROR_SUCCESS) {
                printf("Could not delete old gateway\n");
                goto cleanup;
            }
        }
    }

    /* dwForwardIfIndex filled earlier */
    ipInterfaceRow.Family = AF_INET;
    ipInterfaceRow.InterfaceIndex = ipfRow.dwForwardIfIndex;
    dwRetVal = GetIpInterfaceEntry(&ipInterfaceRow);
    if (dwRetVal != NO_ERROR) {
        fprintf(stderr, "GetIpInterfaceEntry failed with error %d\n", dwRetVal);
        goto cleanup;
    }


    ipfRow.dwForwardDest = 0; // default gateway (0.0.0.0)
    ipfRow.dwForwardMask = 0;
    ipfRow.dwForwardNextHop = gateway;
    ipfRow.dwForwardProto = MIB_IPPROTO_NETMGMT;
    ipfRow.dwForwardMetric1 = ipInterfaceRow.Metric;

    // Create a new route entry for the default gateway.
    dwRetVal = CreateIpForwardEntry(&ipfRow);

    if (dwRetVal != NO_ERROR)
        fprintf(stderr, "CreateIpForwardEntry failed with %d\n", dwRetVal);

    dwRetVal = ERROR_SUCCESS;

cleanup:
    if (pIpForwardTable)
        free(pIpForwardTable);
    if (pAdapterInfo)
        free(pAdapterInfo);

    return dwRetVal;
}



int qubes_setup_network() {
    struct xs_handle *xs;
    int interface_index;
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

    if (set_network_parameters(inet_addr(qubes_ip),
                inet_addr(qubes_netmask),
                inet_addr(qubes_gateway),
                &interface_index) != ERROR_SUCCESS) {
        /* error already reported */
        goto cleanup;
    }

    /* don't know how to programatically (and easily) set DNS address, so stay
     * with netsh... */
    _snprintf(cmdline, sizeof(cmdline), "netsh interface ipv4 set dnsservers \"%d\" static %s register=none validate=no",
            interface_index, qubes_gateway);
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

int __cdecl main(int argc, char **argv) {
    if (argc >= 2 && 0==strcmp(argv[1], "-service")) {
        return service_main();
    } else {
        return qubes_setup_network();
    }
}
