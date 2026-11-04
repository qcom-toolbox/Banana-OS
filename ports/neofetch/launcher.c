/* SPDX-License-Identifier: MIT */
/* neofetch for Banana OS: the real neofetch is a bash script; this runs it
 * (/apps/neofetch/neofetch.sh) with bash and the arguments it was given. */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char** argv) {
    char** av = calloc((size_t)argc + 2, sizeof(char*));
    if (!av) return 1;
    av[0] = "bash";
    av[1] = "/apps/neofetch/neofetch.sh";
    for (int i = 1; i < argc; i++) av[i + 1] = argv[i];
    execv("/bin/bash", av);
    perror("neofetch: /bin/bash");
    return 127;
}
