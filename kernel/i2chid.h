#ifndef I2CHID_H
#define I2CHID_H

/*
 * Touchpads on I2C (HID over I2C): what most laptops since ~2015 have
 * (Synaptics, ELAN, ALPS, Cirque... "precision touchpads"), behind the
 * Intel LPSS DesignWare I2C controllers. Found at boot from the ACPI
 * tables; read by polling (mouse_read).
 */

void i2chid_init(void);
void i2chid_poll(void);
/* "Synaptics I2C precision touchpad (06cb:7e7e)"; 0 if there is none */
int  i2chid_describe(char* out, int cap);

#endif
