/* tones - sound from an app: synthesizes a tune into PCM samples and
 * queues it on the sound card (falls back to PC speaker beeps) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <banana.h>

#define RATE 22050

typedef struct { const char* name; int hz; } note_t;
static const note_t NOTES[] = {
    { "C4", 262 }, { "D4", 294 }, { "E4", 330 }, { "F4", 349 }, { "G4", 392 },
    { "A4", 440 }, { "B4", 494 }, { "C5", 523 }, { "D5", 587 }, { "E5", 659 }, { "-", 0 },
};

static int freq(const char* n) {
    for (unsigned i = 0; i < sizeof(NOTES) / sizeof(NOTES[0]); i++)
        if (strcmp(NOTES[i].name, n) == 0) return NOTES[i].hz;
    return 0;
}

/* a triangle-ish wave with a soft decay, so it sounds less harsh than a square */
static void synth(short* out, int n, int hz) {
    if (!hz) { memset(out, 0, (size_t)n * 2); return; }
    int period = RATE / hz;
    for (int i = 0; i < n; i++) {
        int ph = i % period;
        int tri = ph < period / 2 ? (ph * 4 * 9000 / period) - 9000 : 9000 - ((ph - period / 2) * 4 * 9000 / period);
        int env = 1000 - i * 700 / n;               /* decay to 30% */
        if (i < 200) env = env * i / 200;           /* attack */
        out[i] = (short)(tri * env / 1000);
    }
}

int main(int argc, char** argv) {
    /* Ode to Joy, or the notes given on the command line */
    static const char* ode[] = { "E4", "E4", "F4", "G4", "G4", "F4", "E4", "D4", "C4", "C4", "D4", "E4", "E4", "D4", "D4", "-",
                                 "E4", "E4", "F4", "G4", "G4", "F4", "E4", "D4", "C4", "C4", "D4", "E4", "D4", "C4", "C4" };
    const char** tune = ode;
    int count = (int)(sizeof(ode) / sizeof(ode[0]));
    if (argc > 1) { tune = (const char**)(argv + 1); count = argc - 1; }

    const int note_ms = 280;
    int per = RATE * note_ms / 1000;
    short* buf = malloc((size_t)per * 2);
    if (!buf) return 1;
    int card = 1;
    printf("Playing %d notes: ", count);
    for (int i = 0; i < count; i++) {
        int hz = freq(tune[i]);
        printf("%s ", tune[i]);
        if (card) {
            synth(buf, per, hz);
            if (banana_play(buf, (unsigned long)per * 2, RATE, 1, 16) != 0) {
                card = 0;
                printf("\n(no sound card - using the PC speaker)\n");
            }
        }
        if (!card) {
            if (hz) banana_beep(hz, note_ms - 30);
            banana_sleep(30);
        }
    }
    while (card && banana_playing()) banana_sleep(50);
    printf("\n");
    free(buf);
    return 0;
}
