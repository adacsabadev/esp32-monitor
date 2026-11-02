#include <stdint.h>
#include "ulp_riscv.h"
#include "ulp_riscv_utils.h"

/* Exported to the main CPU as ulp_counter / ulp_period_ms. */
volatile uint32_t counter;
volatile uint32_t period_ms = 10;

int main(void)
{
    while (1) {
        counter++;
        ulp_riscv_delay_cycles(period_ms * ULP_RISCV_CYCLES_PER_MS);
    }
    return 0;
}
