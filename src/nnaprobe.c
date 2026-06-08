/* nnaprobe.c - read-only V831 NPU (NVDLA nv_small) bring-up probe.
 *
 * Maps the CCU (/dev/mem @ 0x03001000) and runs the documented 400 MHz NNA
 * clock/power-on sequence, then maps the NPU register window (@ 0x02400000) and
 * reads its low/CFGROM registers to prove the NPU responds. The only writes are to
 * the CCU clock-select + clock-gate/reset words (the enable sequence); the NPU side
 * is read-only. No model, no ION, no /dev/cedar_dev.
 *
 * Clean-room: uses only documented V831 hardware addresses/values (no third-party
 * source). Goal: confirm (1) /dev/mem mapping works and (2) the reverse-engineered
 * CCU clock magic actually powers the NNA on THIS board's BSP. A non-zero,
 * non-0xFFFFFFFF version word (no bus fault) = success.
 *
 * Must run as root (needs /dev/mem). Build: make nnaprobe.
 */
#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/mman.h>

#define CCU_BASE   0x03001000UL
#define CCU_SIZE   0x1000
#define NNA_BASE   0x02400000UL
#define NNA_SIZE   0x20000

/* CCU u32 word indices for the NNA (reverse-engineered on V831 / MAIX-II BSP) */
#define CCU_NNA_CLK   440          /* byte 0x6E0: clock source/divider select */
#define CCU_NNA_GATE  443          /* byte 0x6EC: bus gate + reset de-assert   */
#define NNA_CLK_400M  0xC1000002u  /* 400 MHz select word                      */
#define NNA_GATE_ON   0x00010001u  /* gate open + reset de-asserted            */

static sigjmp_buf jb;
static void on_fault(int sig){ (void)sig; siglongjmp(jb, 1); }

int main(void){
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if(fd < 0){ perror("open /dev/mem"); return 1; }

    volatile uint32_t *ccu = mmap(0, CCU_SIZE, PROT_READ|PROT_WRITE, MAP_SHARED, fd, CCU_BASE);
    if(ccu == MAP_FAILED){ perror("mmap CCU"); return 1; }

    printf("CCU @0x%08lx before: clk[%d]=0x%08x  gate[%d]=0x%08x\n",
           CCU_BASE, CCU_NNA_CLK, ccu[CCU_NNA_CLK], CCU_NNA_GATE, ccu[CCU_NNA_GATE]);

    /* enable: select 400 MHz, assert reset, then de-assert reset + open gate */
    ccu[CCU_NNA_CLK]  = NNA_CLK_400M;
    ccu[CCU_NNA_GATE] = 0;
    usleep(2000);
    ccu[CCU_NNA_GATE] = NNA_GATE_ON;
    usleep(2000);

    printf("CCU @0x%08lx after:  clk[%d]=0x%08x  gate[%d]=0x%08x\n",
           CCU_BASE, CCU_NNA_CLK, ccu[CCU_NNA_CLK], CCU_NNA_GATE, ccu[CCU_NNA_GATE]);

    volatile uint32_t *nna = mmap(0, NNA_SIZE, PROT_READ|PROT_WRITE, MAP_SHARED, fd, NNA_BASE);
    if(nna == MAP_FAILED){ perror("mmap NNA"); return 1; }

    /* guard NPU reads: if the clock/power magic is wrong, the access bus-faults
     * instead of returning data -- catch it and report rather than crash/hang. */
    signal(SIGBUS,  on_fault);
    signal(SIGSEGV, on_fault);

    if(sigsetjmp(jb, 1) == 0){
        printf("NNA regs @0x%08lx:\n", NNA_BASE);
        for(int off = 0x0000; off <= 0x0020; off += 4)
            printf("  [0x%04x] = 0x%08x\n", off, nna[off/4]);
        printf("RESULT: NPU registers READABLE (no bus fault).\n");
    } else {
        printf("RESULT: BUS FAULT reading NPU regs -- clock/power not enabled "
               "(CCU magic likely wrong for this BSP).\n");
    }

    munmap((void*)nna, NNA_SIZE);
    munmap((void*)ccu, CCU_SIZE);
    close(fd);
    return 0;
}
