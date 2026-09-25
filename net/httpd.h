#ifndef HTTPD_H
#define HTTPD_H

#include "net.h"

/*
 * A small static web server: serves the files under /var/www (GET and
 * HEAD, HTTP/1.1 with Connection: close), with directory listings when a
 * folder has no index.html. One background task, one request at a time.
 */

#define HTTPD_ROOT "/var/www"

int  httpd_start(uint16_t port, char* err, int errcap);   /* 0 or -1 (err says why) */
void httpd_stop(void);
int  httpd_running(void);
uint16_t httpd_port(void);
void httpd_print_status(void);

#endif
