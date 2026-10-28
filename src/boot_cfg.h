/*
 * boot_cfg.h  -  flash layout + boot record, SHARED by bootloader and app
 * Board: NXP MR-CANHUBK344 (S32K344, 4 MB code flash = 4 blocks x 1 MB, 8 KB sectors)
 *
 *  Block 0  0x00400000  BOOT     256 KB used (block is 1 MB)   <- bootloader project
 *  Block 1  0x00500000  APP      1 MB                          <- application project
 *  Block 2  0x00600000  STORAGE  1 MB   OTA staging area (app writes new firmware here)
 *  Block 3  0x00700000  INFO     8 KB   boot_cfg_t record (one sector)
 *
 * Why this split (same idea as the STM32H743 version, which uses 2 banks):
 *  - the C40 flash controller cannot read a block while it is being erased/programmed,
 *    so code that erases/programs must run from a DIFFERENT block than its target:
 *      bootloader (block 0) programs APP (block 1) and INFO (block 3)
 *      application (block 1) programs STORAGE (block 2) and INFO (block 3)
 *  - the last 200 KB of block 3 belong to the HSE firmware - never touch them.
 */
#ifndef BOOT_CFG_H
#define BOOT_CFG_H

#include "Std_Types.h"

#define BL_KB(x)            ((uint32)(x) * 1024UL)

#define BL_SECTOR_SIZE      0x2000UL            /* 8 KB, minimum erase size           */
#define BL_PAGE_SIZE        128UL               /* max program size, 128-byte aligned */

#define BL_BOOT_START       0x00400000UL
#define BL_BOOT_SIZE        BL_KB(256)
#define BL_BOOT_END         (BL_BOOT_START + BL_BOOT_SIZE)

#define BL_APP_START        0x00500000UL
#define BL_APP_SIZE         BL_KB(1024)
#define BL_APP_END          (BL_APP_START + BL_APP_SIZE)
/* startup_cm7.s puts .boot_header at the image start, linker aligns the vector table to 8 KB */
#define BL_APP_VECTOR       (BL_APP_START + 0x2000UL)

#define BL_STORAGE_START    0x00600000UL
#define BL_STORAGE_SIZE     BL_APP_SIZE
#define BL_STORAGE_END      (BL_STORAGE_START + BL_STORAGE_SIZE)

#define BL_INFO_START       0x00700000UL
#define BL_INFO_SIZE        BL_SECTOR_SIZE
#define BL_INFO_END         (BL_INFO_START + BL_INFO_SIZE)

/* RAM ranges where an initial stack pointer may point (S32K344) */
#define BL_DTCM_START       0x20000000UL
#define BL_DTCM_END         0x20020000UL
#define BL_SRAM_START       0x20400000UL
#define BL_SRAM_END         0x20444000UL

/* boot_cfg_t.state */
#define BOOT_STATE_IDLE         0x00000000UL
#define BOOT_STATE_NEW_FW       0x11111111UL   /* new image complete in STORAGE, copy it */
#define BOOT_STATE_COPY_FAIL    0xEEEEEEEEUL

/* Exactly 32 bytes = 4 x 8-byte flash words (flash is programmed in 8-byte units) */
typedef struct
{
    uint32 app;          /* must equal BL_APP_START     */
    uint32 storage;      /* must equal BL_STORAGE_START */
    uint32 state;        /* BOOT_STATE_*                */
    uint32 version;      /* firmware version of the staged image */
    uint32 image_size;   /* bytes of the staged image (multiple of 8) */
    uint32 reserved[3];
} boot_cfg_t;

typedef char bl_assert_cfg_size[(sizeof(boot_cfg_t) == 32U) ? 1 : -1];
typedef char bl_assert_boot_app[(BL_BOOT_END <= BL_APP_START) ? 1 : -1];
typedef char bl_assert_app_storage[(BL_APP_END <= BL_STORAGE_START) ? 1 : -1];
typedef char bl_assert_storage_info[(BL_STORAGE_END <= BL_INFO_START) ? 1 : -1];

#endif /* BOOT_CFG_H */
