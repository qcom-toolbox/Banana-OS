#ifndef KPTEST_H
#define KPTEST_H
/* kernel/kptest.c: worker tasks decode a picture and draw an SVG at once for
 * secs seconds, preempted inside that code; 0 if every result matched */
int kptest_run(const char* img_path, const char* svg_path, int secs, char* out, int cap);
#endif
