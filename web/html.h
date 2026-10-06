#ifndef HTML_H
#define HTML_H

#include "types.h"
#include "arena.h"

/*
 * DOM and a forgiving HTML parser (tag soup in, tree out): implied end
 * tags for p/li/td/..., void elements, raw-text <script>/<style>,
 * character entities. All nodes live in the page's arena.
 */

enum { DOM_DOC = 1, DOM_ELEM, DOM_TEXT, DOM_COMMENT };

typedef struct dom_attr {
    char*  name;                 /* lowercase */
    char*  value;
    struct dom_attr* next;
} dom_attr_t;

struct style;
struct img_data;

typedef struct dom_handler {
    char*  event;                /* "click", "submit", "input", ... */
    void*  fn;                   /* script function (value_t*), set by jsdom */
    struct dom_handler* next;
} dom_handler_t;

typedef struct dom_node {
    uint8_t  type;
    char*    tag;                /* lowercase (elements) */
    char*    text;               /* text/comment data */
    uint32_t text_len;
    dom_attr_t* attrs;
    struct dom_node *parent, *first, *last, *next, *prev;

    struct style* style;         /* computed (css.c) */
    struct img_data* img;        /* decoded <img> */
    uint32_t svg_color;          /* inline <svg>: the text colour img was drawn in (currentColor) */
    struct img_data* bg_img;     /* its CSS background-image (page.c), NULL if none */
    const char* bg_img_url;      /* ...and where it came from */
    void*    js;                 /* cached script wrapper */
    dom_handler_t* handlers;
    char*    value;              /* form controls: current value */
    int      checked;
    int      form_init;          /* value/checked hold the live state (else: from the markup) */
    int      script_done;        /* <script> already run */
    uint8_t  ce_state;           /* custom element: 1 constructed, 2 connected */
    int      box_x, box_y, box_w, box_h;   /* last layout position (page coordinates) */
    uint32_t meas_gen;           /* layout.c: max/min content widths measured in layout pass meas_gen */
    int      meas_max, meas_min;
    uint32_t mh_gen;             /* layout.c: outer height at border-box width mh_w, measured in pass mh_gen */
    int      mh_w, mh;
} dom_node_t;

dom_node_t* html_parse(arena_t* A, const char* src, uint32_t len);
/* parses markup and appends the nodes to parent (innerHTML, document.write) */
void        html_parse_into(arena_t* A, dom_node_t* parent, const char* src, uint32_t len);

dom_node_t* dom_new_element(arena_t* A, const char* tag);
dom_node_t* dom_new_text(arena_t* A, const char* s, uint32_t n);
void        dom_append(dom_node_t* parent, dom_node_t* child);
void        dom_insert_before(dom_node_t* parent, dom_node_t* child, dom_node_t* ref);
void        dom_remove(dom_node_t* n);
void        dom_remove_children(dom_node_t* n);

const char* dom_attr(const dom_node_t* n, const char* name);     /* NULL if absent */
void        dom_set_attr(arena_t* A, dom_node_t* n, const char* name, const char* value);
void        dom_remove_attr(dom_node_t* n, const char* name);
int         dom_has_class(const dom_node_t* n, const char* cls);

dom_node_t* dom_find_tag(dom_node_t* root, const char* tag);    /* first, depth-first */
dom_node_t* dom_find_id(dom_node_t* root, const char* id);
dom_node_t* dom_body(dom_node_t* doc);

/* text of a subtree (textContent) and markup (innerHTML / outerHTML) */
char*       dom_text(arena_t* A, dom_node_t* n);
char*       dom_html(arena_t* A, dom_node_t* n, int outer);

/* decodes &amp; &lt; &#65; &eacute; ... (returns length) */
uint32_t    html_decode_entities(char* s, uint32_t n);

#endif
