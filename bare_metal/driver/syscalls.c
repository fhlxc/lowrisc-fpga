// See LICENSE for license details.

#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>

#include "mini-printf.h"
#include "encoding.h"
#include "memory.h"
#include "bits.h"
#include "hid.h"
#include "lowrisc_memory_map.h"
#include "eth.h"

#define SYS_write 64
#define SYS_exit 93
#define SYS_stats 1234

// initialized in crt.S
int have_vec;

#define static_assert(cond) switch(0) { case 0: case !!(long)(cond): ; }

// In setStats, we might trap reading uarch-specific counters.
// The trap handler will skip over the instruction and write 0,
// but only if a0 is the destination register.
#define read_csr_safe(reg) ({ register long __tmp asm("a0"); \
  asm volatile ("csrr %0, " #reg : "=r"(__tmp)); \
  __tmp; })

#define NUM_COUNTERS 18
static long counters[NUM_COUNTERS];
static char* counter_names[NUM_COUNTERS];
static int handle_stats(int enable)
{
  int i = 0;
#define READ_CTR(name) do { \
    while (i >= NUM_COUNTERS) ; \
    long csr = read_csr_safe(name); \
    if (!enable) { csr -= counters[i]; counter_names[i] = #name; } \
    counters[i++] = csr; \
  } while (0)
  READ_CTR(mcycle);  READ_CTR(minstret);
  READ_CTR(0xcc0); READ_CTR(0xcc1); READ_CTR(0xcc2); READ_CTR(0xcc3);
  READ_CTR(0xcc4); READ_CTR(0xcc5); READ_CTR(0xcc6); READ_CTR(0xcc7);
  READ_CTR(0xcc8); READ_CTR(0xcc9); READ_CTR(0xcca); READ_CTR(0xccb);
  READ_CTR(0xccc); READ_CTR(0xccd); READ_CTR(0xcce); READ_CTR(0xccf);
#undef READ_CTR
  return 0;
}

void tohost_exit(long code)
{
  // halt
  if(code) {
    char str[] = "error! exit(0xFFFFFFFFFFFFFFFF)\n";
    int i;
    for (i = 0; i < 16; i++) {
      str[29-i] = (code & 0xF) + ((code & 0xF) < 10 ? '0' : 'a'-10);
      code >>= 4;
    }
    hid_send_string(str);
  }
  hid_send_string("tohost_exit was called, and this version does not return\n");
  for(;;)
    ;
}

void external_interrupt(void);

// #define VERBOSE_INTERRUPT

void handle_interrupt(long cause)
{
  int mip;
  char code[20];
  cause &= 0x7FFFFFFF;
#ifdef VERBOSE_INTERRUPT
  switch(cause)
    {
    case IRQ_S_SOFT   : strcpy(code, "IRQ_S_SOFT   "); break;
    case IRQ_H_SOFT   : strcpy(code, "IRQ_H_SOFT   "); break;
    case IRQ_M_SOFT   : strcpy(code, "IRQ_M_SOFT   "); break;
    case IRQ_S_TIMER  : strcpy(code, "IRQ_S_TIMER  "); break;
    case IRQ_H_TIMER  : strcpy(code, "IRQ_H_TIMER  "); break;
    case IRQ_M_TIMER  : strcpy(code, "IRQ_M_TIMER  "); break;
    case IRQ_S_EXT    : strcpy(code, "IRQ_S_EXT    "); break;
    case IRQ_H_EXT    : strcpy(code, "IRQ_H_EXT    "); break;
    case IRQ_M_EXT    : strcpy(code, "IRQ_M_EXT    "); break;
    case IRQ_COP      : strcpy(code, "IRQ_COP      "); break;
    case IRQ_HOST     : strcpy(code, "IRQ_HOST     "); break;
    default           : snprintf(code, sizeof(code), "IRQ_%x     ", cause);
    }
 hid_send_string(code);
 mip = read_csr(mip);
 snprintf(code, sizeof(code), "mip=%x\n", mip);
 hid_send_string(code);
#endif
 if (cause==IRQ_M_EXT)
   external_interrupt();  
}

long handle_trap(long cause, long epc, long regs[32])
{
  int* csr_insn;
    // do some report
    
  printf("mepc=%x\n", epc);
  switch(cause)
    {
    case 0:
      printf("mcause=misalign\n");
      break;
    case 1:
      printf("mcause=instr access fault\n");
      break;
    case 2:
      printf("mcause=instr illegal\n");
      break;
    case 3:
      printf("mcause=breakpoint\n");
      break;
    case 4:
      printf("mcause=load address misaligned\n");
      break;
    case 5:
      printf("mcause=load access fault\n");
      break;
    case 6:
      printf("mcause=store address misaligned\n");
      break;
    case 7:
      printf("mcause=store access fault\n");
      break;
    case 8:
      printf("mcause=Environment call from U-mode\n");
      break;
    case 9:
      printf("mcause=Environment call from S-mode\n");
      break;
    case 10:
      printf("mcause=Environment call from H-mode\n");
      break;
    case 11:
      printf("mcause=Environment call from M-mode\n");
      break;
    default:
      printf("mcause=%d (0x%x)\n", cause, cause);
      break;
    }
  
    printf("mbadaddr=%x\n", read_csr_safe(mbadaddr));
    //    printf("einsn=%x\n", *(int*)epc);
    for (int i = 0; i < 32; i++)
      {
        if (i == 2)
          printf("sp =%x ", regs[i]);
        else if (i == 1)
          printf("ra =%x ", regs[i]);
        else if (i == 2)
          printf("sp =%x ", regs[i]);
        else if (i == 3)
          printf("gp =%x ", regs[i]);
        else if ((i >= 10) && (i <= 17))
          printf("a%d=%x ", i-10, regs[i]);
        else if ((i >= 18) && (i <= 27))
          printf("s%d=%x ", i-16, regs[i]);
        else
          printf("x%d=%x ", i, regs[i]);
        if ((i&3) == 3) printf("\n"); 
      }
    tohost_exit(1337);
}

size_t err = 0, eth = 0, ddr = 0, rom = 0, bram = 0, intc = 0, clin = 0, hid = 0;

uint16_t __bswap_16(uint16_t x)
{
	return ((x << 8) & 0xff00) | ((x >> 8) & 0x00ff);
}

uint32_t __bswap_32(uint32_t x)
{
  return
     ((((x) & 0xff000000) >> 24) | (((x) & 0x00ff0000) >>  8) |		      \
      (((x) & 0x0000ff00) <<  8) | (((x) & 0x000000ff) << 24)) ;
}

uip_eth_addr mac_addr;

void _init(int cid, int nc)
{
  extern int main(int, char **);
  int ret = main(0, 0);
  printf("normal exit reached, code=%d\n", ret);
  for(;;);
}

void printhex(uint64_t x)
{
  char str[17];
  int i;
  for (i = 0; i < 16; i++)
  {
    str[15-i] = (x & 0xF) + ((x & 0xF) < 10 ? '0' : 'a'-10);
    x >>= 4;
  }
  str[16] = 0;

  hid_send_string(str);
}

static inline void printnum(void (*putch)(int, void**), void **putdat,
                    unsigned long long num, unsigned base, int width, int padc)
{
  unsigned digs[sizeof(num)*CHAR_BIT];
  int pos = 0;

  while (1)
  {
    digs[pos++] = num % base;
    if (num < base)
      break;
    num /= base;
  }

  while (width-- > pos)
    putch(padc, putdat);

  while (pos-- > 0)
    putch(digs[pos] + (digs[pos] >= 10 ? 'a' - 10 : '0'), putdat);
}

