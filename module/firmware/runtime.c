/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Private freestanding C primitives, not a second filesystem engine.
 * The existing QNX4 engine performs all allocations via the Common host. */
#include <stddef.h>
#include <stdint.h>
void *memcpy(void *d,const void *s,size_t n){unsigned char *a=d;const unsigned char *b=s;while(n--)*a++=*b++;return d;}
void *memset(void *d,int c,size_t n){unsigned char *a=d;while(n--)*a++=(unsigned char)c;return d;}
void *memmove(void *d,const void *s,size_t n){unsigned char *a=d;const unsigned char *b=s;if((uintptr_t)a<=(uintptr_t)b){while(n--)*a++=*b++;}else while(n){--n;a[n]=b[n];}return d;}
int memcmp(const void *a,const void *b,size_t n){const unsigned char *p=a,*q=b;while(n--){int v=(int)*p++-(int)*q++;if(v)return v;}return 0;}
size_t strlen(const char *s){const char *p=s;while(*p)++p;return (size_t)(p-s);}
int strcmp(const char *a,const char *b){while(*a&&*a==*b){++a;++b;}return (unsigned char)*a-(unsigned char)*b;}
int strncmp(const char *a,const char *b,size_t n){while(n&&*a&&*a==*b){++a;++b;--n;}return n?(unsigned char)*a-(unsigned char)*b:0;}
static void swap(unsigned char *a,unsigned char *b,size_t n){while(n--){unsigned char t=*a;*a++=*b;*b++=t;}}
static void sift(unsigned char *p,size_t root,size_t n,size_t width,int (*cmp)(const void *,const void *))
{
    while(root<n/2){size_t child=root*2+1;if(child+1<n&&cmp(p+child*width,p+(child+1)*width)<0)++child;
        if(cmp(p+root*width,p+child*width)>=0)return;
        swap(p+root*width,p+child*width,width);root=child;}
}
void qsort(void *base,size_t count,size_t width,int (*cmp)(const void *,const void *))
{
    unsigned char *p=base;if(!p||!width||count<2||count>SIZE_MAX/width||!cmp)return;
    for(size_t i=count/2;i;)sift(p,--i,count,width,cmp);
    for(size_t i=count;i>1;){--i;swap(p,p+i*width,width);sift(p,0,i,width,cmp);}
}
