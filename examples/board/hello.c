/* kbdk-example
 * name: Hello (hard-float)
 * desc: printf + sqrt smoke test — proves the FPU / hard-float ABI works
 * extra_args: -lm
 */
/* Derived from src/hello.c. The smallest possible board program: it prints a
 * line and computes sqrt(2.0). If this runs and prints the right number, your
 * cross toolchain, the hard-float (NEON/VFPv4) ABI, and musl printf all work.
 *
 * sqrt() lives in libm, which is why the metadata's extra_args carries -lm.
 * (On musl libm is folded into libc, but -lm is still accepted and harmless.) */
#include <stdio.h>
#include <math.h>

int main(void){
    double x = sqrt(2.0);
    printf("hello from KidBright uAI: sqrt(2)=%.5f\n", x);
    return 0;
}
