/* ============================================================================
 * sd_spi.h  --  Minimal SD-card-over-SPI driver for Xilinx AXI Quad SPI
 *
 *  Target : VEGA AT1051 SoC, axi_quad_spi_1 @ 0x10115000
 *  CS     : driven by the SPI core's own Slave-Select register (SSR @ 0x70)
 *
 *  This is a bare-metal-style driver running from Linux user space via
 *  /dev/mem mmap. It implements just enough of the SD SPI protocol to
 *  initialise a card and read/write single 512-byte blocks.
 * ========================================================================== */
#ifndef SD_SPI_H
#define SD_SPI_H

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>

/* ----------------------------------------------------------------------------
 * 1. HARDWARE MAPPING
 * -------------------------------------------------------------------------- */
#define SPI_PHYS_BASE   0x10115000UL   /* axi_quad_spi_1 (from AddressSegments) */
#define MAP_SIZE        4096UL
#define MAP_MASK        (MAP_SIZE - 1)

/* ----------------------------------------------------------------------------
 * 2. XILINX AXI QUAD SPI REGISTER OFFSETS (standard, from PG153 / DS843)
 * -------------------------------------------------------------------------- */
#define XSPI_SRR        0x40   /* Software Reset Register   (write 0x0A)        */
#define XSPI_CR         0x60   /* SPI Control Register                          */
#define XSPI_SR         0x64   /* SPI Status Register                           */
#define XSPI_DTR        0x68   /* SPI Data Transmit Register                    */
#define XSPI_DRR        0x6C   /* SPI Data Receive Register                     */
#define XSPI_SSR        0x70   /* Slave Select Register (active low)            */
#define XSPI_TXFIFO_OCY 0x74   /* Tx FIFO Occupancy                             */
#define XSPI_RXFIFO_OCY 0x78   /* Rx FIFO Occupancy                             */

/* SPI Control Register (CR) bits */
#define CR_LOOP         0x0001
#define CR_SPE          0x0002 /* SPI system enable                             */
#define CR_MASTER       0x0004 /* Master mode                                   */
#define CR_CPOL         0x0008
#define CR_CPHA         0x0010
#define CR_TXFIFO_RST   0x0020 /* Reset Tx FIFO                                 */
#define CR_RXFIFO_RST   0x0040 /* Reset Rx FIFO                                 */
#define CR_MANUAL_SS    0x0080 /* Manual slave-select (we drive SSR ourselves)  */
#define CR_TRANS_INHIBIT 0x0100/* Master transaction inhibit                    */

/* SPI Status Register (SR) bits */
#define SR_RX_EMPTY     0x0001
#define SR_RX_FULL      0x0002
#define SR_TX_EMPTY     0x0004
#define SR_TX_FULL      0x0008

/* ----------------------------------------------------------------------------
 * 3. SD PROTOCOL CONSTANTS
 * -------------------------------------------------------------------------- */
#define SD_BLOCK_LEN    512

/* SD commands (SPI mode) */
#define CMD0    0   /* GO_IDLE_STATE        */
#define CMD8    8   /* SEND_IF_COND         */
#define CMD9    9   /* SEND_CSD             */
#define CMD16   16  /* SET_BLOCKLEN         */
#define CMD17   17  /* READ_SINGLE_BLOCK    */
#define CMD24   24  /* WRITE_BLOCK          */
#define CMD55   55  /* APP_CMD              */
#define CMD58   58  /* READ_OCR             */
#define ACMD41  41  /* SD_SEND_OP_COND      */

#define R1_IDLE_STATE   0x01
#define R1_READY        0x00
#define DATA_START_TOKEN 0xFE
#define DATA_ACCEPTED    0x05  /* (resp & 0x1F) after a write */

/* ----------------------------------------------------------------------------
 * 4. GLOBALS + LOW-LEVEL REGISTER ACCESS
 * -------------------------------------------------------------------------- */
static volatile uint32_t *spi_base_virt = NULL;
static int sd_is_sdhc = 0;   /* 1 = block addressing, 0 = byte addressing */

#define SPI_REG(off) (*(volatile uint32_t *)((uint8_t *)spi_base_virt + (off)))

static void *map_physical_memory(uint32_t phys_addr)
{
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd == -1) { perror("open /dev/mem"); exit(EXIT_FAILURE); }
    void *base = mmap(0, MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
                      fd, phys_addr & ~MAP_MASK);
    if (base == MAP_FAILED) { perror("mmap"); close(fd); exit(EXIT_FAILURE); }
    close(fd);
    return (void *)((uint8_t *)base + (phys_addr & MAP_MASK));
}

/* ----------------------------------------------------------------------------
 * 5. SPI PRIMITIVES
 * -------------------------------------------------------------------------- */

/* Bring the core up in master / manual-SS mode, CS de-asserted. */
static void spi_init(void)
{
    SPI_REG(XSPI_SRR) = 0x0000000A;          /* software reset */
    usleep(1000);

    SPI_REG(XSPI_SSR) = 0xFFFFFFFF;          /* all slaves de-selected */

    /* Reset both FIFOs, master, manual SS, inhibit transactions for now.
     * CPOL=0, CPHA=0 (SD mode 0). */
    SPI_REG(XSPI_CR) = CR_MASTER | CR_MANUAL_SS | CR_TXFIFO_RST |
                       CR_RXFIFO_RST | CR_TRANS_INHIBIT;

    /* Enable the core, keep transactions inhibited until we load data. */
    SPI_REG(XSPI_CR) = CR_MASTER | CR_MANUAL_SS | CR_SPE | CR_TRANS_INHIBIT;
}

static void cs_assert(void)   { SPI_REG(XSPI_SSR) = 0xFFFFFFFE; } /* select slave 0 */
static void cs_deassert(void) { SPI_REG(XSPI_SSR) = 0xFFFFFFFF; }

/* Full-duplex byte exchange. Returns the byte clocked back from MISO. */
static uint8_t spi_xfer(uint8_t out)
{
    uint32_t cr;
    int guard;

    /* Drain any stale rx data. */
    while (!(SPI_REG(XSPI_SR) & SR_RX_EMPTY))
        (void)SPI_REG(XSPI_DRR);

    SPI_REG(XSPI_DTR) = out;                 /* load tx byte */

    /* Release the transaction inhibit so the byte is clocked out. */
    cr = SPI_REG(XSPI_CR);
    SPI_REG(XSPI_CR) = cr & ~CR_TRANS_INHIBIT;

    /* Wait for the receive byte to appear (one byte was sent). */
    guard = 0;
    while (SPI_REG(XSPI_SR) & SR_RX_EMPTY) {
        if (++guard > 1000000) break;        /* hardware-stuck safety net */
    }

    /* Re-inhibit so the next DTR load doesn't auto-fire. */
    cr = SPI_REG(XSPI_CR);
    SPI_REG(XSPI_CR) = cr | CR_TRANS_INHIBIT;

    return (uint8_t)SPI_REG(XSPI_DRR);
}

/* Convenience: clock out 0xFF (idle) and capture the response byte. */
static uint8_t spi_rx(void) { return spi_xfer(0xFF); }

/* ----------------------------------------------------------------------------
 * 6. SD CARD PROTOCOL
 * -------------------------------------------------------------------------- */

/* Send a command frame and return the R1 response byte (0xFF on timeout). */
static uint8_t sd_command(uint8_t cmd, uint32_t arg)
{
    uint8_t crc = 0xFF;
    uint8_t r1;
    int i;

    /* CRC is only mandatory for CMD0 and CMD8 in SPI mode. */
    if (cmd == CMD0) crc = 0x95;             /* valid CRC for arg 0          */
    if (cmd == CMD8) crc = 0x87;             /* valid CRC for arg 0x1AA      */

    /* A leading dummy byte gives the card a clock to finish prior work. */
    spi_rx();

    spi_xfer(0x40 | cmd);                    /* start bit + command index   */
    spi_xfer((arg >> 24) & 0xFF);
    spi_xfer((arg >> 16) & 0xFF);
    spi_xfer((arg >> 8)  & 0xFF);
    spi_xfer( arg        & 0xFF);
    spi_xfer(crc);

    /* R1 comes back within 1-8 bytes; top bit clears when valid. */
    for (i = 0; i < 8; i++) {
        r1 = spi_rx();
        if (!(r1 & 0x80)) return r1;
    }
    return 0xFF;
}

/* CMD55 + ACMDxx wrapper. */
static uint8_t sd_acommand(uint8_t acmd, uint32_t arg)
{
    sd_command(CMD55, 0);
    return sd_command(acmd, arg);
}

/*
 * Initialise the card. Returns 0 on success, negative on failure.
 * Sets sd_is_sdhc so read/write know whether to use block or byte addressing.
 */
static int sd_init(void)
{
    int i;
    uint8_t r1;

    spi_init();

    /* >= 74 clocks with CS HIGH so the card enters native SPI mode. */
    cs_deassert();
    for (i = 0; i < 10; i++) spi_rx();       /* 10 bytes = 80 clocks */

    /* CMD0: go idle. Expect R1 = 0x01. Retry a few times. */
    cs_assert();
    for (i = 0; i < 10; i++) {
        r1 = sd_command(CMD0, 0);
        if (r1 == R1_IDLE_STATE) break;
        usleep(1000);
    }
    if (r1 != R1_IDLE_STATE) {
        cs_deassert();
        printf("  [sd] CMD0 failed (r1=0x%02X) -- card not responding.\n", r1);
        return -1;
    }

    /* CMD8: voltage check. 0x1AA = 2.7-3.6V, check pattern 0xAA. */
    r1 = sd_command(CMD8, 0x000001AA);
    if (r1 == R1_IDLE_STATE) {
        uint8_t ocr[4];
        for (i = 0; i < 4; i++) ocr[i] = spi_rx();   /* 32-bit trailing data */
        if (ocr[2] != 0x01 || ocr[3] != 0xAA) {
            cs_deassert();
            printf("  [sd] CMD8 echo mismatch (%02X %02X).\n", ocr[2], ocr[3]);
            return -2;
        }
        /* v2 card path: ACMD41 with HCS bit set. */
        for (i = 0; i < 2000; i++) {
            r1 = sd_acommand(ACMD41, 0x40000000);
            if (r1 == R1_READY) break;
            usleep(1000);
        }
        if (r1 != R1_READY) {
            cs_deassert();
            printf("  [sd] ACMD41 (v2) timeout (r1=0x%02X).\n", r1);
            return -3;
        }
        /* CMD58: read OCR, check CCS bit (bit 30) for SDHC/SDXC. */
        r1 = sd_command(CMD58, 0);
        if (r1 == R1_READY) {
            uint8_t ocr2[4];
            for (i = 0; i < 4; i++) ocr2[i] = spi_rx();
            sd_is_sdhc = (ocr2[0] & 0x40) ? 1 : 0;
        }
    } else {
        /* v1 card (or MMC) path: plain ACMD41. */
        for (i = 0; i < 2000; i++) {
            r1 = sd_acommand(ACMD41, 0);
            if (r1 == R1_READY) break;
            usleep(1000);
        }
        if (r1 != R1_READY) {
            cs_deassert();
            printf("  [sd] ACMD41 (v1) timeout (r1=0x%02X).\n", r1);
            return -4;
        }
        sd_is_sdhc = 0;
    }

    /* Standard-capacity cards: force 512-byte block length. SDHC ignores. */
    if (!sd_is_sdhc) sd_command(CMD16, SD_BLOCK_LEN);

    cs_deassert();
    spi_rx();   /* trailing clocks to let the card release the bus */
    return 0;
}

/* Translate a logical block number to the card's argument convention. */
static uint32_t sd_addr(uint32_t block)
{
    return sd_is_sdhc ? block : (block * SD_BLOCK_LEN);
}

/* Write one 512-byte block. buf must point to SD_BLOCK_LEN bytes. */
static int sd_write_block(uint32_t block, const uint8_t *buf)
{
    int i;
    uint8_t r1, resp;

    cs_assert();
    r1 = sd_command(CMD24, sd_addr(block));
    if (r1 != R1_READY) {
        cs_deassert();
        printf("  [sd] CMD24 rejected (r1=0x%02X).\n", r1);
        return -1;
    }

    spi_rx();                       /* one idle byte before the token */
    spi_xfer(DATA_START_TOKEN);     /* 0xFE: start of data block       */

    for (i = 0; i < SD_BLOCK_LEN; i++)
        spi_xfer(buf[i]);

    spi_xfer(0xFF);                 /* dummy CRC (ignored in SPI mode) */
    spi_xfer(0xFF);

    /* Data response token: xxx0sss1, sss=010 means accepted. */
    resp = spi_rx();
    if ((resp & 0x1F) != DATA_ACCEPTED) {
        cs_deassert();
        printf("  [sd] write not accepted (token=0x%02X).\n", resp);
        return -2;
    }

    /* Card holds MISO low while it programs flash; wait for it to release. */
    for (i = 0; i < 1000000; i++) {
        if (spi_rx() == 0xFF) break;
        usleep(10);
    }

    cs_deassert();
    spi_rx();
    return 0;
}

/* Read one 512-byte block into buf (must hold SD_BLOCK_LEN bytes). */
static int sd_read_block(uint32_t block, uint8_t *buf)
{
    int i;
    uint8_t r1, token;

    cs_assert();
    r1 = sd_command(CMD17, sd_addr(block));
    if (r1 != R1_READY) {
        cs_deassert();
        printf("  [sd] CMD17 rejected (r1=0x%02X).\n", r1);
        return -1;
    }

    /* Wait for the data-start token 0xFE. */
    token = 0xFF;
    for (i = 0; i < 100000; i++) {
        token = spi_rx();
        if (token == DATA_START_TOKEN) break;
        if (token != 0xFF) {        /* an error token (bits set) was returned */
            cs_deassert();
            printf("  [sd] read error token 0x%02X.\n", token);
            return -2;
        }
    }
    if (token != DATA_START_TOKEN) {
        cs_deassert();
        printf("  [sd] read token timeout.\n");
        return -3;
    }

    for (i = 0; i < SD_BLOCK_LEN; i++)
        buf[i] = spi_rx();

    spi_rx();                       /* discard 16-bit CRC */
    spi_rx();

    cs_deassert();
    spi_rx();
    return 0;
}

#endif /* SD_SPI_H */
