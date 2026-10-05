/* guess - a console game: input with scanf, colors, random numbers */
#include <stdio.h>
#include <banana.h>

int main(void) {
    banana_color(BANANA_C_YELLOW, BANANA_C_BLACK);
    printf("== Guess the Number ==\n");
    banana_color(BANANA_C_LIGHT_GREY, BANANA_C_BLACK);

    for (;;) {
        int secret = (int)(banana_random() % 100) + 1;
        int tries = 0, guess = 0;
        printf("I picked a number between 1 and 100.\n");
        while (guess != secret) {
            printf("Your guess: ");
            if (scanf("%d", &guess) != 1) {
                int c;
                while ((c = getchar()) != '\n' && c != EOF) ;   /* skip the bad line */
                printf("Type a number, please.\n");
                continue;
            }
            tries++;
            if (guess < secret) { banana_color(BANANA_C_LIGHT_CYAN, BANANA_C_BLACK); printf("  higher!\n"); }
            else if (guess > secret) { banana_color(BANANA_C_LIGHT_MAGENTA, BANANA_C_BLACK); printf("  lower!\n"); }
            banana_color(BANANA_C_LIGHT_GREY, BANANA_C_BLACK);
        }
        banana_color(BANANA_C_LIGHT_GREEN, BANANA_C_BLACK);
        printf("Yes, %d! You found it in %d tr%s.\n", secret, tries, tries == 1 ? "y" : "ies");
        banana_color(BANANA_C_LIGHT_GREY, BANANA_C_BLACK);
        printf("Again? (y/n) ");
        char answer[8];
        if (scanf("%7s", answer) != 1 || (answer[0] != 'y' && answer[0] != 'Y')) break;
    }
    printf("Bye!\n");
    return 0;
}
