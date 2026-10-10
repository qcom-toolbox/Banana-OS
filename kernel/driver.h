#ifndef DRIVER_H
#define DRIVER_H

#include "types.h"

/*
 * Driver modules (the Banana OS Driver Kit: sdk/driver/banana_driver.h):
 * hardware drivers built outside Banana OS, installed as .bpk packages of
 * type "driver" and loaded into the kernel at boot.
 */

/* loads one (an ELF made with the Driver Kit) and runs its entry point; 0 or -1 */
int  driver_load(const char* path, const char* name, char* err, int ecap);
/* an installed driver package (/apps/<name>/driver-<arch>) */
int  driver_load_package(const char* name, char* err, int ecap);
/* every installed one (boot, once the files are there) */
void drivers_load_installed(void);
void drivers_list(void);                 /* `drivers` */

#endif
