/* hello.c - smoke-test for the KidBright µAI (Allwinner V831, Cortex-A7).
 * Exercises musl printf and the FPU (sqrt) so we know hard-float works. */
#include <stdio.h>
#include <math.h>

int main(void){
    double x = sqrt(2.0);
    printf("hello from KidBright uAI: sqrt(2)=%.5f\n", x);
    return 0;
}
