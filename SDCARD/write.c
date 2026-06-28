/* ============================================================================
 * write.c  --  SD-over-SPI write test
 *
 *  Compile (on host):
 *    riscv32-linux-gcc write.c -o write -static -lm
 *    riscv32-linux-strip write
 *
 *  Run (on board, as root):
 *    chmod +x write
 *    ./write
 *
 *  It initialises the SD card on axi_quad_spi_1, writes a known string into
 *  TEST_BLOCK, then exits. Run ./read afterwards to read it back.
 * ========================================================================== */
#include "sd_spi.h"

#define TEST_BLOCK   100u          /* logical block number to use for the test */

static const char *TEST_STRING = "hi this is a write test";

int main(void)
{
    uint8_t block[SD_BLOCK_LEN];

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

    /* Build the 512-byte block: string + NUL, rest zero-filled. */
    memset(block, 0x00, SD_BLOCK_LEN);
    strncpy((char *)block, TEST_STRING, SD_BLOCK_LEN - 1);

    printf("[STEP 2] Writing block %u: \"%s\"\n", TEST_BLOCK, TEST_STRING);
    if (sd_write_block(TEST_BLOCK, block) != 0) {
        printf("[FAIL] Block write failed.\n");
        return -1;
    }

    printf("[DONE] Write completed. You can now run ./read\n");
    return 0;
}
