/* nncls.c (stub) - prove we can dlopen the board's libmaix_nn.so from plain C. */
#include <stdio.h>
#include <dlfcn.h>

extern void *aw_dlsym(void *handle, const char *symbol) __asm__("dlsym");
#define NN_SO "/usr/lib/python3.8/site-packages/maix/libmaix_nn.so"

/* libmaix_nn.so carries undefined back-references to decoder hooks that normally live
 * in the _maix_nn Python extension. musl always binds immediately (RTLD_LAZY is a
 * no-op), so these must resolve at load. We never use those decoders, so we export
 * harmless stubs from this executable (built with -Wl,--export-dynamic) for the loader
 * to bind against. */
void retinaface_get_priorboxes(void){}
void retinaface_decode(void){}

int main(void){
    void *h = dlopen(NN_SO, RTLD_NOW|RTLD_GLOBAL);
    if(!h){ fprintf(stderr,"dlopen %s: %s\n", NN_SO, dlerror()); return 1; }
    void *p = aw_dlsym(RTLD_DEFAULT, "libmaix_nn_create");
    printf("dlopen OK; libmaix_nn_create=%p\n", p);
    return p ? 0 : 1;
}
