/*
 * boot_flash.h - small flash helper on top of the NXP RTD C40_Ip driver.
 * Used by BOTH the bootloader and the application.
 * The calling code must NOT run from the same 1 MB flash block it erases/programs (see boot_cfg.h).
 */
#ifndef NXP_BOOT_FLASH_API_H
#define NXP_BOOT_FLASH_API_H

#include "Std_Types.h"
#include "boot_cfg.h"

boolean BootFlash_Init(void);

/* addr must be sector aligned; size is rounded up to whole 8 KB sectors */
boolean BootFlash_Erase(uint32 addr, uint32 size);

/* Program + verify. addr must be 8-byte aligned. len is rounded up to a multiple of 8 (padded 0xFF). */
boolean BootFlash_Write(uint32 addr, const uint8 *data, uint32 len);

/* ECC-safe read of flash into RAM (does not fault on erased / bad ECC data) */
boolean BootFlash_Read(uint32 addr, uint8 *dst, uint32 len);

/* TRUE if [addr, addr+len) is fully erased (0xFF) */
boolean BootFlash_IsBlank(uint32 addr, uint32 len);

/* D-cache / I-cache maintenance for a flash range that was just erased or programmed */
void    BootFlash_CacheInvalidate(uint32 addr, uint32 len);

/* boot record in the INFO sector */
boolean BootCfg_Load(boot_cfg_t *cfg);          /* TRUE = a valid record was found, else cfg = defaults */
boolean BootCfg_Save(const boot_cfg_t *cfg);
boolean BootCfg_RequestOta(uint32 version, uint32 image_size);   /* used by the application */

/* TRUE if vector table at 'vec' has a sane SP and a Thumb reset handler inside the APP area */
boolean Boot_IsValidAppVector(uint32 vec);

uint16  Boot_Crc16(const uint8 *data, uint32 len);   /* CRC16-CCITT-FALSE (poly 0x1021, init 0xFFFF) */

#endif /* NXP_BOOT_FLASH_API_H */
