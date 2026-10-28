/*!
 * BOOTLOADER main.c  -  NXP MR-CANHUBK344 (S32K344)
 * Equivalent of the STM32H743 bootloader (bootloader.c):
 *
 *   reset -> read boot record (INFO sector)
 *     state == NEW_FW : validate image in STORAGE, erase APP, copy STORAGE -> APP, verify,
 *                       clear the state, software reset
 *     otherwise       : validate the vector table of the APP and jump to it
 *   no valid app      : blink the green LED (fast) and wait - flash an app with the debugger / run OTA again
 *
 * Linker script : Project_Settings/Linker_Files/linker_flash_s32k344_boot.ld   (BOOT = 0x00400000, 256 KB)
 * Shared files  : common/boot_cfg.h, common/boot_flash.[ch]
 *
 * NOT COMPILED OR RUN - written against the RTD 6.0.0 headers of the MRCANHUBK344 project.
 *
 * Design notes
 *  - The jump to the app is done BEFORE Clock_Ip_Init / pin init whenever possible, so the app starts from a
 *    chip state very close to a normal reset (clock = FIRC, no peripheral configured).  After a copy we do a
 *    software reset instead of jumping, for the same reason.
 *  - Flash writes use the C40_Ip driver (common/boot_flash.c).  This code runs from block 0, the targets
 *    (APP = block 1, INFO = block 3) are in other blocks (read-while-write).
 */
#include "Mcal.h"
#include "Clock_Ip.h"
#include "Siul2_Port_Ip.h"
#include "Siul2_Dio_Ip.h"
#include "boot_cfg.h"
#include "boot_flash.h"

/* Prototypes repeated here on purpose: some S32DS projects pick up a different/empty boot_flash.h first.
 * Identical declarations may be repeated in C, so this is harmless when the header is the right one. */
extern boolean BootFlash_Init(void);
extern boolean BootFlash_Erase(uint32 addr, uint32 size);
extern boolean BootFlash_Write(uint32 addr, const uint8 *data, uint32 len);
extern boolean BootFlash_Read(uint32 addr, uint8 *dst, uint32 len);
extern boolean BootFlash_IsBlank(uint32 addr, uint32 len);
extern boolean BootCfg_Load(boot_cfg_t *cfg);
extern boolean BootCfg_Save(const boot_cfg_t *cfg);
extern boolean BootCfg_RequestOta(uint32 version, uint32 image_size);
extern boolean Boot_IsValidAppVector(uint32 vec);
extern uint16  Boot_Crc16(const uint8 *data, uint32 len);

/* ---- Cortex-M7 registers used for the jump (no CMSIS dependency) ---- */
#define REG32(a)            (*(volatile uint32 *)(a))
#define SCB_CCR             REG32(0xE000ED14UL)
#define SCB_AIRCR           REG32(0xE000ED0CUL)
#define SCB_VTOR            REG32(0xE000ED08UL)
#define SCB_CCSIDR          REG32(0xE000ED80UL)
#define SCB_CSSELR          REG32(0xE000ED84UL)
#define SCB_ICIALLU         REG32(0xE000EF50UL)
#define SCB_DCISW           REG32(0xE000EF60UL)
#define SCB_BPIALL          REG32(0xE000EF78UL)
#define MPU_CTRL            REG32(0xE000ED94UL)
#define SYST_CSR            REG32(0xE000E010UL)
#define NVIC_ICER(n)        REG32(0xE000E180UL + 4UL * (n))
#define NVIC_ICPR(n)        REG32(0xE000E280UL + 4UL * (n))
#define CCR_IC              (1UL << 17)
#define CCR_DC              (1UL << 16)

#define DSB()               __asm volatile ("dsb 0xF" ::: "memory")
#define ISB()               __asm volatile ("isb 0xF" ::: "memory")

#define COPY_RETRIES        3U

/* ---- diagnostics: watch these in the debugger (Live Expressions) ----
 * g_blStage : 1 start, 2 flash init done, 3 cfg loaded, 4 jumping to app, 5 copying, 6 copy ok, 7 cfg saved, 9 halted
 * g_blFail  : 0 ok, else the blink code below
 * LED blink codes when the bootloader stops (N short blinks, pause, repeat):
 *   1 flash driver init failed      2 staged image in STORAGE invalid     3 erase of APP failed
 *   4 copy / program / verify failed   5 no valid application in flash    6 saving the boot record failed */
volatile uint32 g_blStage = 0U;
volatile uint32 g_blFail  = 0U;
extern volatile uint32 g_bfErr;       /* from boot_flash.c: last C40 error (step<<8 | status) */

static uint8 s_buf[BL_PAGE_SIZE] __attribute__((aligned(8)));
static uint8 s_buf2[BL_PAGE_SIZE] __attribute__((aligned(8)));

static void Led(boolean on)
{
    /* LED is active high on PTA27 in the example project; flip here if your board is inverted */
    Siul2_Dio_Ip_WritePin(LED_GREEN_PORT, LED_GREEN_PIN, on ? 1U : 0U);
}

static void DelayLoops(uint32 n) { for (volatile uint32 i = 0U; i < n; i++) { } }

/* Blink 'code' times, long pause, repeat forever (needs clock + pins initialised) */
static void Fail_Blink(uint32 code)
{
    g_blStage = 9U;
    g_blFail  = code;
    for (;;)
    {
        for (uint32 i = 0U; i < code; i++) { Led(TRUE); DelayLoops(1500000UL); Led(FALSE); DelayLoops(1500000UL); }
        DelayLoops(9000000UL);
    }
}

static void SoftReset(void)
{
    DSB();
    SCB_AIRCR = 0x05FA0004UL;          /* VECTKEY | SYSRESETREQ */
    DSB();
    for (;;) { }
}

/* Switch D-cache/I-cache/MPU off and clear everything the bootloader may have touched, so the app's
 * startup code finds the core the way it is after a reset.                                              */
static void PrepareForJump(void)
{
    uint32 i;

    __asm volatile ("cpsid i" ::: "memory");
    SYST_CSR = 0U;
    for (i = 0U; i < 8U; i++) { NVIC_ICER(i) = 0xFFFFFFFFUL; NVIC_ICPR(i) = 0xFFFFFFFFUL; }

    /* D-cache: nothing worth cleaning (we only read through it), so disable + invalidate by set/way */
    DSB(); ISB();
    if ((SCB_CCR & CCR_DC) != 0U)
    {
        uint32 ccsidr, sets, ways, set, way;
        SCB_CSSELR = 0U;
        DSB();
        ccsidr = SCB_CCSIDR;
        SCB_CCR &= ~CCR_DC;
        DSB();
        sets = ((ccsidr >> 13) & 0x7FFFUL) + 1U;
        ways = ((ccsidr >> 3)  & 0x3FFUL)  + 1U;
        /* cache line = 32 bytes -> set index starts at bit 5; way index in the top bits */
        for (set = 0U; set < sets; set++)
        {
            for (way = 0U; way < ways; way++)
            {
                SCB_DCISW = (set << 5) | (way << 30);
            }
        }
        DSB(); ISB();
    }
    if ((SCB_CCR & CCR_IC) != 0U)
    {
        SCB_CCR &= ~CCR_IC;
        DSB();
        SCB_ICIALLU = 0U;
        SCB_BPIALL  = 0U;
        DSB(); ISB();
    }
    MPU_CTRL = 0U;                      /* app's system.c programs its own MPU table */
    DSB(); ISB();
}

/* Jump to the application: vector table = [SP, Reset_Handler, ...] at BL_APP_VECTOR */
static void JumpToApp(void)
{
    const uint32 sp    = *(volatile const uint32 *)(BL_APP_VECTOR);
    const uint32 entry = *(volatile const uint32 *)(BL_APP_VECTOR + 4UL);

    PrepareForJump();
    SCB_VTOR = BL_APP_VECTOR;
    DSB(); ISB();
    __asm volatile ("msr msp, %0" :: "r" (sp) : );
    ((void (*)(void))entry)();
    for (;;) { }
}

/* Copy STORAGE -> APP.  Returns 0 if the APP now holds an exact copy, else 3 (erase failed) / 4 (copy failed). */
static uint32 CopyStorageToApp(uint32 image_size)
{
    uint32 off;

    if (!BootFlash_Erase(BL_APP_START, image_size)) { return 3U; }

    for (off = 0U; off < image_size; off += BL_PAGE_SIZE)
    {
        uint32 n = image_size - off;
        if (n > BL_PAGE_SIZE) { n = BL_PAGE_SIZE; }

        if (!BootFlash_Read(BL_STORAGE_START + off, s_buf, n))            { return 4U; }
        if (!BootFlash_Write(BL_APP_START + off, s_buf, n))               { return 4U; }   /* write verifies */
        if (!BootFlash_Read(BL_APP_START + off, s_buf2, n))               { return 4U; }
        for (uint32 k = 0U; k < n; k++) { if (s_buf[k] != s_buf2[k]) { return 4U; } }
    }
    return 0U;
}

static void InitLedPins(void)
{
    Clock_Ip_Init(&Mcu_aClockConfigPB[0]);
    Siul2_Port_Ip_Init(NUM_OF_CONFIGURED_PINS_PortContainer_0_BOARD_InitPeripherals,
                       g_pin_mux_InitConfigArr_PortContainer_0_BOARD_InitPeripherals);
}

int main(void)
{
    boot_cfg_t cfg;
    boolean    haveCfg;
    boolean    appValid, stagedValid, pending;

    g_blStage = 1U;
    if (!BootFlash_Init())
    {
        /* flash driver failed: try to run whatever is in APP, otherwise stop */
        if (Boot_IsValidAppVector(BL_APP_VECTOR)) { JumpToApp(); }
        InitLedPins();
        Fail_Blink(1U);
    }
    g_blStage = 2U;

    haveCfg     = BootCfg_Load(&cfg);
    g_blStage   = 3U;
    appValid    = Boot_IsValidAppVector(BL_APP_VECTOR);
    stagedValid = haveCfg && (cfg.image_size != 0U) && (cfg.image_size <= BL_APP_SIZE) && ((cfg.image_size & 7U) == 0U) &&
                  Boot_IsValidAppVector(BL_STORAGE_START + (BL_APP_VECTOR - BL_APP_START));

    /* copy when the app asked for it (NEW_FW), or when an earlier copy failed and there is no usable app (retry) */
    pending = haveCfg && ((cfg.state == BOOT_STATE_NEW_FW) || ((cfg.state == BOOT_STATE_COPY_FAIL) && !appValid));

    /* ---------- normal start: nothing to copy -> straight to the app ---------- */
    if (!pending && appValid)
    {
        g_blStage = 4U;
        JumpToApp();
    }

    /* From here we need the LED (copy pending, or no valid app) */
    InitLedPins();
    Led(TRUE);                                           /* solid ON while copying */

    if (pending)
    {
        uint32 result = 2U;                              /* 2 = staged image invalid */
        uint32 t;

        g_blStage = 5U;
        if (stagedValid)
        {
            for (t = 0U; t < COPY_RETRIES; t++)
            {
                result = CopyStorageToApp(cfg.image_size);
                if (result == 0U) { break; }
            }
        }
        g_blFail = result;

        cfg.app         = BL_APP_START;
        cfg.storage     = BL_STORAGE_START;
        cfg.state       = (result == 0U) ? BOOT_STATE_IDLE : BOOT_STATE_COPY_FAIL;
        cfg.reserved[0] = result;                        /* the application reports these in CONNECT / CHECK_INFO */
        cfg.reserved[1] = g_bfErr;
        g_blStage = 6U;
        if (!BootCfg_Save(&cfg)) { if (result == 0U) { Fail_Blink(6U); } }   /* if the record cannot be cleared we would copy again forever */
        g_blStage = 7U;

        if (result == 0U) { SoftReset(); }               /* next boot jumps from a clean chip state */
        Fail_Blink(result);
    }

    Fail_Blink(5U);                                      /* no valid application in flash */
    return 0;
}
