#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "panel_ppa_guard.h"
int main(void) {
    uint16_t small[]={1,2,3,4,5,6}, got[6]={0}, expected[]={3,6,2,5,1,4};
    if(!panel_ppa_guard_copy_rgb565_rot90(small,got,3,2,0,0,3,2,2,3,0,0) || memcmp(got,expected,sizeof(got))) return 1;
    const unsigned hs[]={1,2,3,15};
    for(unsigned k=0;k<4;k++) {
        unsigned h=hs[k], iw=1300, ih=h+2, ox=62;
        uint16_t *src=calloc((size_t)iw*ih,2), *dst=malloc(800u*1280u*2);
        if(!src||!dst) return 2;
        for(unsigned y=0;y<ih;y++) for(unsigned x=0;x<iw;x++) src[(size_t)y*iw+x]=(uint16_t)(1+x+257*y);
        for(unsigned n=0;n<800u*1280u;n++) dst[n]=0xAAAA;
        if(!panel_ppa_guard_copy_rgb565_rot90(src,dst,iw,ih,7,1,1280,h,800,1280,ox,0)) return 3;
        for(unsigned dy=0;dy<1280;dy++) for(unsigned dx=0;dx<800;dx++) {
            uint16_t want=0xAAAA;
            if(dx>=ox && dx<ox+h) want=src[(size_t)(dx-ox+1)*iw+(1279-dy+7)];
            if(dst[(size_t)dy*800+dx]!=want) {fprintf(stderr,"mismatch h%u x%u y%u\n",h,dx,dy); return 4;}
        }
        free(src); free(dst);
    }
    puts("PASS independent known CCW matrix and full 800x1280 inverse-map pixels, h=1/2/3/15, padded stride, offsets, untouched neighbors");
    return 0;
}
