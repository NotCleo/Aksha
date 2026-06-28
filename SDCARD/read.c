/* ============================================================================
 * read.c  --  SD-over-SPI read test
 *
 *  Compile (on host):
 *    riscv32-linux-gcc read.c -o read -static -lm
 *    riscv32-linux-strip read
 *
 *  Run (on board, as root):
 *    chmod +x read
 *    ./read
 *
 *  It initialises the SD card on axi_quad_spi_1, reads TEST_BLOCK back and
 *  prints the string that ./write stored there.
 * ========================================================================== */
#include "sd_spi.h"
#include <ctype.h>

#define TEST_BLOCK   100u          /* MUST match write.c */

int main(void)
{
    uint8_t block[SD_BLOCK_LEN];
    int i, printable;

    printf("[INIT] Mapping SPI controller @ 0x%08lX ...\n",
           (unsigned long)SPI_PHYS_BASE);
    spi_base_virt = (volatile uint32_t *)map_physical_memory(SPI_PHYS_BASE);

    printf("[STEP 1] Initialising SD card ...\n");
    if (sd_init() != 0) {
        printf("[FAIL] SD init failed. Check wiring, power (5V), and SCK <= 400kHz during init.\n");
        return -1;
    }
    printf("  ok SPI good, card type: %s\n", sd_is_sdhc ? "SDHC/SDXC (block addr)"
                                                        : "SDSC (byte addr)");

    printf("[STEP 2] Reading block %u ...\n", TEST_BLOCK);
    memset(block, 0, SD_BLOCK_LEN);
    if (sd_read_block(TEST_BLOCK, block) != 0) {
        printf("[FAIL] Block read failed.\n");
        return -1;
    }

    /* Print as a C string (write.c NUL-terminated it). */
    printf("[DONE] Read complete, here is content:\n");
    printf("  \"%s\"\n", (char *)block);

    /* Also dump the first 32 bytes in hex so you can confirm raw contents. */
    printf("  hex: ");
    for (i = 0; i < 32; i++) printf("%02X ", block[i]);
    printf("\n  asc: ");
    for (i = 0; i < 32; i++) {
        printable = block[i];
        putchar(isprint(printable) ? printable : '.');
    }
    printf("\n");

    return 0;
}
