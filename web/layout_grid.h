/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Explicit column tracks and automatic rows. No item-count cap: only one row
 * lives on the stack. Measurement never publishes DOM hit-test rectangles. */
static int layout_grid(ctx_t* C, dom_node_t* e, int x, int y, int w) {
    const style_t* st=e->style;
    for(dom_node_t* c=e->first;c;c=c->next)
        if(c->type==DOM_TEXT && !is_blank_text(c)) {
            if(st->align_items!=FA_CENTER)return layout_children(C,e,x,y,w);
            /* place-items: center around a text (an icon's letter, a badge's
             * number): centered across and, in a box of given height, down */
            int saved=C->dry,saved_w=C->max_w;
            C->dry=1;
            int ch=layout_children(C,e,x,y,w);
            C->dry=saved;C->max_w=saved_w;
            int box=st->height!=LEN_AUTO && st->height>0?st->height:ch,dy=box>ch?(box-ch)/2:0;
            style_t* ms=(style_t*)st;
            uint8_t align=ms->align;
            ms->align=ALIGN_CENTER;
            int h=layout_children(C,e,x,y+dy,w);
            ms->align=align;
            return h+dy;
        }
    int n=st->grid_ncols?st->grid_ncols:1;
    /* Bound track arithmetic as well as the number of tracks. Huge CSS values
     * outside the drawable range must not wrap signed pixel coordinates. */
    if(w>1000000)w=1000000;
    int gap_col=st->gap_col>1000000?1000000:st->gap_col;
    int gap_row=st->gap_row>1000000?1000000:st->gap_row;
    int widths[GRID_MAX_COLS]={0},weights[GRID_MAX_COLS]={0};
    int free=w-(n-1)*gap_col,weight=0;
    for(int i=0;i<n;i++) {
        grid_track_t t=st->grid_ncols?st->grid_cols[i]:(grid_track_t){100,0,GRID_FR};
        if(t.kind==GRID_FR) { weights[i]=t.value;weight+=t.value; }
        else { int64_t size=t.kind==GRID_PERCENT?(int64_t)w*t.value/100:t.value;
            widths[i]=size>1000000?1000000:(int)size;
            if(widths[i]<t.min)widths[i]=t.min;
            free-=widths[i]; }
    }
    if(C->dry && w>=50000) {
        /* Max-content measurement of fractional tracks uses their content,
         * rather than the deliberately enormous measurement viewport. */
        int col=0;
        for(dom_node_t* c=e->first;c;c=c->next) {
            if(c->type!=DOM_ELEM || !c->style || c->style->display==DISP_NONE || is_out_of_flow(c) || out_of_sight(c->style))continue;
            if(weights[col]) { int mx,mn;measure(C,c,&mx,&mn);mx+=c->style->margin[1]+c->style->margin[3];if(mx>widths[col])widths[col]=mx; }
            col=(col+1)%n;
        }
        for(int i=0;i<n;i++)if(st->grid_ncols && widths[i]<st->grid_cols[i].min)widths[i]=st->grid_cols[i].min;
    } else {
        /* Freeze minmax lower bounds before distributing the remaining fr. */
        for(int pass=0;pass<n;pass++) {
            int changed=0;
            for(int i=0;i<n;i++)if(weights[i] && st->grid_ncols &&
                (int64_t)(free>0?free:0)*weights[i] < (int64_t)st->grid_cols[i].min*weight) {
                widths[i]=st->grid_cols[i].min;free-=widths[i];weight-=weights[i];weights[i]=0;changed=1;
            }
            if(!changed)break;
        }
        if(free<0)free=0;
        for(int i=0;i<n;i++)if(weights[i]) {
            widths[i]=weight?(int)((int64_t)free*weights[i]/weight):0;
            free-=widths[i];weight-=weights[i];
        }
    }
    int total=(n-1)*gap_col;
    for(int i=0;i<n;i++)total+=widths[i];
    dom_node_t* next=e->first;int cy=y,rows=0;
    while(next) {
        dom_node_t* items[GRID_MAX_COLS];int heights[GRID_MAX_COLS],count=0,rh=0;
        while(next && count<n) {
            dom_node_t* c=next;next=next->next;
            if(c->type!=DOM_ELEM || !c->style || c->style->display==DISP_NONE || out_of_sight(c->style))continue;
            if(is_out_of_flow(c)){defer_abs(C,c,x,cy);continue;}
            items[count++]=c;
        }
        if(!count)break;
        if(rows++)cy+=gap_row;
        for(int i=0;i<count;i++) {
            const style_t* s=items[i]->style;
            int bw=widths[i]-s->margin[1]-s->margin[3];
            if(s->width!=LEN_AUTO)bw=s->width+box_extra_w(s);
            if(s->max_width!=LEN_AUTO && bw>s->max_width+box_extra_w(s))bw=s->max_width+box_extra_w(s);
            heights[i]=flex_measure_h(C,items[i],bw>0?bw:1);
            if(heights[i]>rh)rh=heights[i];
        }
        int cx=x;
        for(int i=0;i<count;i++) {
            const style_t* s=items[i]->style;int iy=cy;
            int align=s->align_self?s->align_self:st->align_items;
            if(align==FA_CENTER)iy+=(rh-heights[i])/2;
            else if(align==FA_END)iy+=rh-heights[i];
            if(!align || align==FA_STRETCH) {
                if(s->height==LEN_AUTO)C->force_h=rh-s->margin[0]-s->margin[2];
            }
            layout_box(C,items[i],cx,iy,widths[i]);
            cx+=widths[i]+gap_col;
        }
        cy+=rh;
    }
    if(C->max_w<total)C->max_w=total;
    return cy-y;
}
