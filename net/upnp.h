#ifndef UPNP_H
#define UPNP_H

#include "net.h"

/*
 * UPnP Internet Gateway Device client: asks the home router to forward a
 * port from the Internet to this machine (what games and BitTorrent
 * clients do), so httpd can be reached from outside. SSDP discovery
 * (multicast and straight to the gateway), the device description, then
 * WANIPConnection / WANPPPConnection SOAP calls.
 */

/* Forwards TCP external_port on the router to our internal_port. On
 * success fills public_ip (the router's Internet address, may be "" if
 * it would not say) and returns 0; otherwise -1 with a reason in err. */
int upnp_add_port(uint16_t external_port, uint16_t internal_port, const char* description,
                  char* public_ip, int ipcap, char* err, int errcap);
int upnp_delete_port(uint16_t external_port, char* err, int errcap);

/* 1 for addresses that are not reachable from the Internet (RFC 1918, CGNAT, ...) */
int upnp_is_private(ip4_t ip);

#endif
