#ifndef ACPI_H
#define ACPI_H

/* Power the machine off the ACPI way (the S5 sleep state, read from the
 * firmware's FADT and DSDT). Returns only if that did not work. */
void acpi_s5_poweroff(void);

#endif
