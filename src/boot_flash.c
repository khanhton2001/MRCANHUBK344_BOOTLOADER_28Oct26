/*
 * boot_flash.c - flash erase/program/verify + boot record, shared by bootloader and application.
 * Uses the RTD C40_Ip driver in "main interface, async" mode (the driver default):
 *    start job -> poll status.
 */
#include "boot_flash.h"
#include "C40_Ip.h"
#include "C40_Ip_Cfg.h"

#define BF_POLL_LIMIT_ERASE   0x10000000UL     /* poll iterations before giving up (sector erase) */
#define BF_POLL_LIMIT_WRITE   0x04000000UL     /* poll iterations before giving up (page program) */
#define BF_DOMAIN_ID          0U               /* core 0 */

/* Cortex-M7 cache maintenance registers (System Control Block) */
#define BF_SCB_ICIALLU        (*(volatile uint32 *)0xE000EF50UL)
#define BF_SCB_DCIMVAC        (*(volatile uint32 *)0xE000EF5CUL)
#define BF_SCB_BPIALL         (*(volatile uint32 *)0xE000EF78UL)
#define BF_CACHE_LINE         32UL

static boolean s_inited = FALSE;

/* Debug: where/why the last flash operation failed (read it in the debugger / Live Expressions).
 *   high byte = step (1 erase start, 2 erase status, 3 write start, 4 write status, 5 verify, 6 unlock), low byte = C40 status */
volatile uint32 g_bfErr = 0UL;

/* Flash sectors are locked after reset: C40_Ip_*Erase/Write return an error unless the sector is unlocked first. */
static boolean BF_Unlock(uint32 addr)
{
    C40_Ip_VirtualSectorsType vs = C40_Ip_GetSectorNumberFromAddress(addr);
    C40_Ip_StatusType st = C40_Ip_GetLock(vs);

    if (st == C40_IP_STATUS_SECTOR_UNPROTECTED) { return TRUE; }
    st = C40_Ip_ClearLock(vs, BF_DOMAIN_ID);
    if ((st != C40_IP_STATUS_SUCCESS) || (C40_Ip_GetLock(vs) != C40_IP_STATUS_SECTOR_UNPROTECTED))
    {
        g_bfErr = (6UL << 8) | ((uint32)st & 0xFFUL);
        return FALSE;
    }
    return TRUE;
}

void BootFlash_CacheInvalidate(uint32 addr, uint32 len)
{
    uint32 a   = addr & ~(BF_CACHE_LINE - 1UL);
    uint32 end = addr + len;

    __asm volatile ("dsb 0xF" ::: "memory");
    for (; a < end; a += BF_CACHE_LINE)
    {
        BF_SCB_DCIMVAC = a;                /* flash lines are never dirty, so invalidate is safe */
    }
    __asm volatile ("dsb 0xF" ::: "memory");
    BF_SCB_ICIALLU = 0UL;
    BF_SCB_BPIALL  = 0UL;
    __asm volatile ("dsb 0xF" ::: "memory");
    __asm volatile ("isb 0xF" ::: "memory");
}

boolean BootFlash_Init(void)
{
    if (s_inited == FALSE)
    {
        if (C40_Ip_Init(&C40_Ip_InitCfg) != C40_IP_STATUS_SUCCESS)
        {
            return FALSE;
        }
        C40_Ip_SetAsyncMode(TRUE);
        s_inited = TRUE;
    }
    return TRUE;
}

boolean BootFlash_Erase(uint32 addr, uint32 size)
{
    uint32 a;
    uint32 end;

    if ((addr % BL_SECTOR_SIZE) != 0UL) { return FALSE; }
    if (size == 0UL)                    { return FALSE; }

    end = addr + ((size + BL_SECTOR_SIZE - 1UL) & ~(BL_SECTOR_SIZE - 1UL));

    for (a = addr; a < end; a += BL_SECTOR_SIZE)
    {
        C40_Ip_StatusType st;
        uint32 guard = BF_POLL_LIMIT_ERASE;

        if (!BF_Unlock(a)) { return FALSE; }
        st = C40_Ip_MainInterfaceSectorErase(C40_Ip_GetSectorNumberFromAddress(a), BF_DOMAIN_ID);
        if (st != C40_IP_STATUS_SUCCESS) { g_bfErr = (1UL << 8) | ((uint32)st & 0xFFUL); return FALSE; }

        do
        {
            st = C40_Ip_MainInterfaceSectorEraseStatus();
            guard--;
        } while ((st == C40_IP_STATUS_BUSY) && (guard != 0UL));

        if (st != C40_IP_STATUS_SUCCESS)
        {
            g_bfErr = (2UL << 8) | ((uint32)st & 0xFFUL);
            (void)C40_Ip_Abort();
            return FALSE;
        }
    }

    BootFlash_CacheInvalidate(addr, end - addr);
    return TRUE;
}

boolean BootFlash_Write(uint32 addr, const uint8 *data, uint32 len)
{
    static uint8 page[BL_PAGE_SIZE] __attribute__((aligned(8)));
    uint32 done = 0UL;

    if (((addr & 7UL) != 0UL) || (data == NULL_PTR) || (len == 0UL)) { return FALSE; }

    while (done < len)
    {
        uint32 a        = addr + done;
        uint32 in_page  = BL_PAGE_SIZE - (a & (BL_PAGE_SIZE - 1UL));   /* bytes left in this 128-byte page */
        uint32 chunk    = len - done;
        uint32 padded;
        uint32 i;
        uint32 guard    = BF_POLL_LIMIT_WRITE;
        C40_Ip_StatusType st;

        if (chunk > in_page) { chunk = in_page; }
        padded = (chunk + 7UL) & ~7UL;                                  /* flash is programmed in 8-byte words */

        for (i = 0UL; i < padded; i++)
        {
            page[i] = (i < chunk) ? data[done + i] : 0xFFU;
        }

        if (!BF_Unlock(a)) { return FALSE; }
        st = C40_Ip_MainInterfaceWrite(a, padded, page, BF_DOMAIN_ID);
        if (st != C40_IP_STATUS_SUCCESS) { g_bfErr = (3UL << 8) | ((uint32)st & 0xFFUL); return FALSE; }

        do
        {
            st = C40_Ip_MainInterfaceWriteStatus();
            guard--;
        } while ((st == C40_IP_STATUS_BUSY) && (guard != 0UL));

        if (st != C40_IP_STATUS_SUCCESS)
        {
            g_bfErr = (4UL << 8) | ((uint32)st & 0xFFUL);
            (void)C40_Ip_Abort();
            return FALSE;
        }

        BootFlash_CacheInvalidate(a, padded);
        if (C40_Ip_Compare(a, padded, page) != C40_IP_STATUS_SUCCESS)
        {
            g_bfErr = (5UL << 8);
            return FALSE;                                               /* read-back verify failed */
        }

        done += chunk;
    }
    return TRUE;
}

boolean BootFlash_Read(uint32 addr, uint8 *dst, uint32 len)
{
    BootFlash_CacheInvalidate(addr, len);
    return (C40_Ip_Read(addr, len, dst) == C40_IP_STATUS_SUCCESS) ? TRUE : FALSE;
}

boolean BootFlash_IsBlank(uint32 addr, uint32 len)
{
    return (C40_Ip_Compare(addr, len, NULL_PTR) == C40_IP_STATUS_SUCCESS) ? TRUE : FALSE;
}

/* ------------------------------------------------------------------------------------------- */
/*  Boot record                                                                                */
/* ------------------------------------------------------------------------------------------- */
static void BootCfg_SetDefault(boot_cfg_t *cfg)
{
    uint32 i;
    uint8 *p = (uint8 *)cfg;
    for (i = 0UL; i < sizeof(*cfg); i++) { p[i] = 0U; }
    cfg->app     = BL_APP_START;
    cfg->storage = BL_STORAGE_START;
    cfg->state   = BOOT_STATE_IDLE;
}

static boolean BootCfg_IsValid(const boot_cfg_t *cfg)
{
    if (cfg->app != BL_APP_START)         { return FALSE; }
    if (cfg->storage != BL_STORAGE_START) { return FALSE; }
    if ((cfg->state != BOOT_STATE_IDLE) &&
        (cfg->state != BOOT_STATE_NEW_FW) &&
        (cfg->state != BOOT_STATE_COPY_FAIL)) { return FALSE; }
    if (cfg->image_size > BL_APP_SIZE)    { return FALSE; }
    return TRUE;
}

boolean BootCfg_Load(boot_cfg_t *cfg)
{
    /* erased sector (never written) -> defaults, without touching erased flash through the cache */
    if (BootFlash_IsBlank(BL_INFO_START, sizeof(*cfg)) == TRUE)
    {
        BootCfg_SetDefault(cfg);
        return FALSE;
    }
    if ((BootFlash_Read(BL_INFO_START, (uint8 *)cfg, sizeof(*cfg)) == FALSE) || (BootCfg_IsValid(cfg) == FALSE))
    {
        BootCfg_SetDefault(cfg);
        return FALSE;
    }
    return TRUE;
}

boolean BootCfg_Save(const boot_cfg_t *cfg)
{
    uint8 attempt;

    if (BootCfg_IsValid(cfg) == FALSE) { return FALSE; }

    for (attempt = 0U; attempt < 3U; attempt++)
    {
        if (BootFlash_Erase(BL_INFO_START, BL_INFO_SIZE) == FALSE) { continue; }
        if (BootFlash_Write(BL_INFO_START, (const uint8 *)cfg, sizeof(*cfg)) == TRUE) { return TRUE; }
    }
    return FALSE;
}

boolean BootCfg_RequestOta(uint32 version, uint32 image_size)
{
    boot_cfg_t cfg;

    BootCfg_SetDefault(&cfg);
    cfg.state      = BOOT_STATE_NEW_FW;
    cfg.version    = version;
    cfg.image_size = (image_size + 7UL) & ~7UL;
    return BootCfg_Save(&cfg);
}

/* ------------------------------------------------------------------------------------------- */
static boolean Boot_IsRamAddr(uint32 a)
{
    if ((a >= BL_DTCM_START) && (a <= BL_DTCM_END)) { return TRUE; }
    if ((a >= BL_SRAM_START) && (a <= BL_SRAM_END)) { return TRUE; }
    return FALSE;
}

boolean Boot_IsValidAppVector(uint32 vec)
{
    uint32 v[2];
    uint32 reset;

    /* blank flash -> no application (and no read of erased cells through the CPU) */
    if (BootFlash_IsBlank(vec, 8UL) == TRUE) { return FALSE; }
    if (BootFlash_Read(vec, (uint8 *)v, 8UL) == FALSE) { return FALSE; }

    if (Boot_IsRamAddr(v[0]) == FALSE) { return FALSE; }          /* initial SP in RAM          */
    if ((v[1] & 1UL) == 0UL)           { return FALSE; }          /* Thumb bit set              */
    reset = v[1] & ~1UL;
    if ((reset < BL_APP_START) || (reset >= BL_APP_END)) { return FALSE; }   /* handler inside APP */
    return TRUE;
}

uint16 Boot_Crc16(const uint8 *data, uint32 len)
{
    uint16 crc = 0xFFFFU;
    uint32 i;
    uint8  b;

    for (i = 0UL; i < len; i++)
    {
        crc ^= (uint16)((uint16)data[i] << 8U);
        for (b = 0U; b < 8U; b++)
        {
            crc = ((crc & 0x8000U) != 0U) ? (uint16)((crc << 1U) ^ 0x1021U) : (uint16)(crc << 1U);
        }
    }
    return crc;
}
