/* Host test: HTML file -> DOM -> CSS -> layout -> pixels (PPM on stdout) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "html.h"
#include "css.h"
#include "layout.h"
#include "render.h"

static int collect_styles(arena_t* A, dom_node_t* n, css_sheet_t** out, int max, int k) {
    for (dom_node_t* c = n->first; c && k < max; c = c->next) {
        if (c->type == DOM_ELEM && strcmp(c->tag, "style") == 0 && c->first)
            out[k++] = css_parse(A, c->first->text, c->first->text_len, 1);
        k = collect_styles(A, c, out, max, k);
    }
    return k;
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: render_test file.html out.ppm [width]\n"); return 2; }
    int width = argc > 3 ? atoi(argv[3]) : 680;
    FILE* f = fopen(argv[1], "rb");
    if (!f) return 2;
    static char src[1 << 20];
    size_t n = fread(src, 1, sizeof(src) - 1, f);
    fclose(f);
    arena_t A;
    arena_init(&A, 0);
    dom_node_t* doc = html_parse(&A, src, (uint32_t)n);
    css_sheet_t* sheets[16];
    sheets[0] = css_default_sheet(&A);
    int ns = 1 + collect_styles(&A, doc, sheets + 1, 15, 0);
    css_style_tree(&A, doc, sheets, ns);
    layout_t* L = layout_build(&A, doc, width, NULL);
    int h = L->height < 100 ? 100 : L->height;
    if (h > 4000) h = 4000;
    uint32_t* buf = calloc((size_t)width * h, 4);
    render_page(L, buf, width, width, h, 0, 0, width, h, 0);
    FILE* o = fopen(argv[2], "wb");
    fprintf(o, "P6\n%d %d\n255\n", width, h);
    for (int i = 0; i < width * h; i++) { unsigned char px[3] = { buf[i] >> 16, buf[i] >> 8, buf[i] }; fwrite(px, 1, 3, o); }
    fclose(o);
    fprintf(stderr, "layout: %u items, height %d\n", L->n, L->height);
    return 0;
}
