/* hello - the smallest Banana OS app */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <banana.h>

int main(int argc, char** argv) {
    banana_time_t t;
    banana_time(&t);

    banana_color(BANANA_C_YELLOW, BANANA_C_BLACK);
    printf("Hello from a Banana OS app!\n");
    banana_color(BANANA_C_LIGHT_GREY, BANANA_C_BLACK);

    printf("  running on:  Banana OS %s, %s kernel\n", banana_api()->os_version, banana_api()->arch);
    printf("  date:        %04d-%02d-%02d %02d:%02d:%02d\n", t.year, t.month, t.day, t.hour, t.minute, t.second);
    printf("  uptime:      %u.%03u s\n", banana_ticks() / 1000, banana_ticks() % 1000);

    char cwd[128];
    if (banana_api()->getcwd(cwd, sizeof(cwd)) == 0) printf("  folder:      %s\n", cwd);

    printf("  arguments:  ");
    for (int i = 0; i < argc; i++) printf(" [%s]", argv[i]);
    printf("\n");

    /* the heap: allocate, use, free */
    char* msg = malloc(64);
    snprintf(msg, 64, "%d + %d = %d", 40, 2, 40 + 2);
    printf("  some math:   %s\n", msg);
    free(msg);

    /* files: write one, read it back */
    FILE* f = fopen("/tmp/hello.txt", "w");
    if (f) {
        fprintf(f, "written by the hello app at %02d:%02d\n", t.hour, t.minute);
        fclose(f);
        char line[80];
        f = fopen("/tmp/hello.txt", "r");
        if (f && fgets(line, sizeof(line), f)) printf("  /tmp/hello.txt: %s", line);
        if (f) fclose(f);
    }
    return 0;
}
