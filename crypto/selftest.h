#ifndef CRYPTO_SELFTEST_H
#define CRYPTO_SELFTEST_H

/* Runs the crypto known-answer tests, calling report() once per test.
 * Returns 1 if everything passed. */
int crypto_selftest(void (*report)(const char* name, int ok));

#endif
