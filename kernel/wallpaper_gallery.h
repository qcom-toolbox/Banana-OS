/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef WALLPAPER_GALLERY_H
#define WALLPAPER_GALLERY_H
#define WP_THUMB_W 80
#define WP_THUMB_H 50
#define WP_THUMB_GAP 8
#define WP_THUMB_ROW 70
typedef struct { int per_row,rows,capacity,pages,pager_h; } wp_gallery_t;
/* h is the content height above the status bar. Reserve the user's pictures
 * and controls before sizing the grid. An empty history needs only its label.
 * Extremely short windows retain at least one navigable preset row; callers
 * omit footer controls that do not fit, using the same drawing/hit bounds. */
static inline wp_gallery_t wp_gallery_layout(int w,int h,int count,int recent) {
    wp_gallery_t l={0};
    l.per_row=(w+WP_THUMB_GAP)/(WP_THUMB_W+WP_THUMB_GAP);
    if(l.per_row<1)l.per_row=1;
    if(count<0)count=0;
    int total=(count+l.per_row-1)/l.per_row;
    if(total<1)total=1;
    int footer=4+18+(recent?WP_THUMB_H:18)+24+44;
    int room=h-38-footer;
    l.rows=room/WP_THUMB_ROW;
    if(l.rows<1)l.rows=1;
    if(l.rows<total){
        l.pager_h=24;l.rows=(room-l.pager_h)/WP_THUMB_ROW;
        if(l.rows<1)l.rows=1;
    }
    if(l.rows>total)l.rows=total;
    l.capacity=l.rows*l.per_row;
    l.pages=(count+l.capacity-1)/l.capacity;
    if(l.pages<1)l.pages=1;
    if(l.pages==1)l.pager_h=0;
    return l;
}
#endif
