# NXP MR-CANHUBK344 (S32K344): bootloader + application + CAN flasher

This is the S32K344 version of the STM32H743 bootloader/app pair.
The application receives a new firmware over CAN into a staging area, the bootloader copies it over the application and starts it.

> **Status:** written from the RTD 6.0.0 sources of your project and checked against your two project zips (sources, ELF layout, linker scripts).
> The firmware has **not been run on a board by me**. Section 9 lists what is still unverified.

---------------------------------------------------------------------------------------------------

## 1. The pieces

| Piece | Where | What it does |
|---|---|---|
| Bootloader | `bootloader/main.c` + `bootloader/linker_flash_s32k344_boot.ld` | Runs first after every reset. Copies a new image from STORAGE into APP when asked, then jumps to the app. |
| Application | `app/main.c` + `app/linker_flash_s32k344_app.ld` | Your CAN test + WS2812 program, plus the CAN OTA service (receives the new firmware). |
| Shared flash code | `common/boot_cfg.h`, `common/boot_flash.h`, `common/boot_flash.c` | Memory map, boot record, erase / program / verify on top of the NXP `C40_Ip` driver. **Same files in both projects.** |
| PC flasher | `host/can_flash_waveshare.py` | Window (or command line) that reads an `.elf`, sends it over the Waveshare USB-CAN (e.g. COM7). |

---------------------------------------------------------------------------------------------------

## 2. Flash map (S32K344, 4 MB code flash)

The code flash is 4 blocks of 1 MB. The smallest erasable unit is a **sector of 8 KB (0x2000)**; a block has 128 sectors.
Programming is done in **8-byte words**, at most **128 bytes (one page)** per command, and the page must not cross a 128-byte boundary.

| Area | Start | End (inclusive) | Size | Sectors (8 KB) | Flash block | Written by |
|---|---|---|---|---|---|---|
| **BOOT** (bootloader) | `0x00400000` | `0x0043FFFF` | 256 KB (reserved) | 32 reserved, ~12 used | block 0 | debugger only |
| *(rest of block 0)* | `0x00440000` | `0x004FFFFF` | 768 KB | - | block 0 | unused |
| **APP** (application) | `0x00500000` | `0x005FFFFF` | 1 MB | 128 (a 96 KB app uses 12) | block 1 | bootloader (copy) or debugger |
| **STORAGE** (OTA staging) | `0x00600000` | `0x006FFFFF` | 1 MB | 128 | block 2 | application, over CAN |
| **INFO** (boot record) | `0x00700000` | `0x00701FFF` | 8 KB | 1 | block 3, first sector | application (request), bootloader (clear) |
| *HSE firmware* | end of block 3 | - | (reserved by NXP) | - | block 3 | **never touch** |

Sector address = `0x00400000 + n * 0x2000`. Examples: APP sector 0 = `0x00500000`, APP sector 1 = `0x00502000`, INFO = `0x00700000`.

Why this split: while a block is being erased or programmed it cannot be read as code, so code that erases/programs must run from a **different** block than its target.
- bootloader (runs in block 0) programs APP (block 1) and INFO (block 3)
- application (runs in block 1) programs STORAGE (block 2) and INFO (block 3)

Constants live in `common/boot_cfg.h` (`BL_APP_START`, `BL_STORAGE_START`, `BL_INFO_START`, ...). Change the map there **and** in the two linker scripts.

### What is inside an image (APP or BOOT)

| Offset in image | Content |
|---|---|
| `+0x0000` | `.boot_header`: IVT marker `0x5AA55AA5` + the vector table address (needed by the boot ROM; for the APP it is just carried along) |
| `+0x2000` | vector table (8 KB aligned): word 0 = initial stack pointer, word 1 = `Reset_Handler` (Thumb) |
| `+0x2420` (typical) | `Reset_Handler` and startup code, then `.text`, `.rodata` |
| end | load images of initialised data (`.data` copied to RAM at start) |

The linker script of the app asserts that the vector table is exactly at `0x00502000`, because the bootloader jumps there. A typical app here is about 96 KB (0x17758 bytes = 12 sectors).

RAM: DTCM `0x20000000-0x2001FFFF`, SRAM `0x20400000-0x20443FFF` (the bootloader accepts an initial stack pointer in either).

---------------------------------------------------------------------------------------------------

## 3. The boot record (INFO sector)

32 bytes at `0x00700000`, defined by `boot_cfg_t`:

| Offset | Field | Meaning |
|---|---|---|
| 0x00 | `app` | must be `0x00500000` |
| 0x04 | `storage` | must be `0x00600000` |
| 0x08 | `state` | `0x00000000` idle/ok, `0x11111111` new firmware waiting in STORAGE, `0xEEEEEEEE` the last copy failed |
| 0x0C | `version` | version number sent by the PC (0-255) |
| 0x10 | `image_size` | bytes in STORAGE (multiple of 8) |
| 0x14 | `reserved[0]` | result of the last bootloader copy: 0 ok, 2 staged image invalid, 3 erase APP failed, 4 copy failed |
| 0x18 | `reserved[1]` | last flash driver error (`g_bfErr`) |
| 0x1C | `reserved[2]` | unused |

An erased INFO sector (all 0xFF) or an invalid record means "idle with default values". The sector is only rewritten when something changes (erase 8 KB, then program 32 bytes).

---------------------------------------------------------------------------------------------------

## 4. How it works, step by step

```
 PC (Waveshare, COM7)                 Application (0x00500000)                 Bootloader (0x00400000)
 ---------------------                ------------------------                 -----------------------
 CONNECT   ------------------------>  answers: version, boot state, last copy result
 ERASE size ----------------------->  erases STORAGE sectors for 'size' bytes
 DATA block 0..N (1 KB each) ------->  checks CRC16, programs STORAGE (128-byte pages), verifies
 FINISH size,crc,version ---------->  CRC16 of the whole STORAGE image, checks the vector table,
                                       writes INFO: state = NEW_FW, image_size, version
                                       answers ACK, then software reset ---------->  reset
                                                                              reads INFO: NEW_FW
                                                                              checks image in STORAGE
                                                                              erases APP sectors
                                                                              copies STORAGE -> APP (128-byte pages),
                                                                              reads back and compares each page
                                                                              INFO: state = idle (result 0)
                                                                              software reset
                                                                              reads INFO: idle, APP vector valid
                                                                              disables IRQ/caches/MPU, sets VTOR + SP
                                                                              jumps to the application  ---------->  app runs
```

### Bootloader decision on every reset (`bootloader/main.c`)
1. Initialise the flash driver, read the boot record, check the APP vector table (SP in RAM, Thumb bit, reset handler inside APP) and the staged image.
2. **Copy pending** (`state = NEW_FW`, or `state = COPY_FAIL` and there is no usable app): LED solid on, up to 3 copy attempts, update INFO, reset.
3. **Otherwise, valid app:** jump to it. (Done before clocks/pins are initialised, so the app starts from a chip state close to a normal reset.)
4. **Otherwise:** stop and blink the failure code.

### Jump to the app
Interrupts off, SysTick off, NVIC cleared, D-cache and I-cache invalidated and switched off, MPU off, `VTOR = 0x00502000`, `MSP` = word 0 of the vector table, branch to word 1 (`Reset_Handler` of the app). The app's own startup code then initialises everything again.

### Flash driver details (`common/boot_flash.c`)
- Uses the `C40_Ip` driver in asynchronous main-interface mode: start the job, then poll until done.
- **Every sector is unlocked first** (`C40_Ip_ClearLock`); sectors are locked after reset and erase/write fail otherwise.
- Erase works per 8 KB sector. Program works in pages of up to 128 bytes, padded with `0xFF` to 8 bytes, then the data is read back and compared (`C40_Ip_Compare`).
- The data cache is invalidated after every erase/program, because the flash controller changes the flash behind the cache.
- Reads of flash that might be erased or not yet programmed use `C40_Ip_Read` / blank check, never a plain CPU read (an ECC error would fault).

---------------------------------------------------------------------------------------------------

## 5. CAN protocol (1 Mbit/s, standard 11-bit IDs, 8 data bytes, little endian)

| ID | Direction | Use |
|---|---|---|
| `0x7F0` | PC -> board | command frame |
| `0x7F1` | board -> PC | response `[cmd, status, d0..d5]` |
| `0x7F2` | PC -> board | raw data frames of a block, 8 bytes each |

Status codes: `0xAC` ACK, `0xE0` generic / CRC / frames missing, `0xE1` wrong state, `0xE2` bad parameter, `0xE3` flash error.

| Cmd | Name | Payload (after the command byte) | Response data |
|---|---|---|---|
| `0xC0` | CONNECT | - | app version (4), boot state (1), last bootloader result (1) |
| `0xC1` | CHECK_INFO | - | version stored in INFO (4), state (1), result (1) |
| `0xC2` | ERASE | image size (4) | erases STORAGE for that size; can take several seconds |
| `0xC3` | DATA | offset (3), length (2), CRC16 of the block (2) | offset (3). The header is followed by `length/8` frames on `0x7F2` |
| `0xC4` | FINISH | image size (4), CRC16 of the whole image (2), version (1) | board resets after the ACK |
| `0xC5` | ABORT | - | back to idle |

Block rules: at most **1024 bytes**, multiple of 8, offset multiple of 8, blocks in order (a repeated block is acknowledged without writing again).
CRC16 is CCITT-FALSE (polynomial 0x1021, init 0xFFFF). Leave **at least 1 ms between the `0x7F2` frames**: the board polls one receive mailbox. A lost frame gives `0xE0` and the PC repeats the block (up to 5 times).

While an OTA session is open the application pauses its normal 100 ms job (CAN 0x555 frame + WS2812) so no data frame is missed; the session closes by itself after 5 s without an OTA command.

Numbers for a 96 KB image: about 94 blocks of 1 KB, 128 data frames per block, roughly 20 s on Windows (the sleep between frames is rounded up by the system).

---------------------------------------------------------------------------------------------------

## 6. PC flasher (`host/can_flash_waveshare.py`)

```
py -3 -m pip install pyserial
py -3 can_flash_waveshare.py                          # opens the window
py -3 can_flash_waveshare.py my_app.elf --port COM7   # command line
```
- Window: **Browse...** for the `.elf` (or `.bin`), COM port list (default COM7), CAN bitrate (default 1000000), serial baud (default 2000000, use 115200 if the adapter does not open), version number, **Flash**.
- It reads the ELF itself: the loadable segments inside `0x00500000-0x005FFFFF` are placed into a byte image starting at `0x00500000`, gaps filled with `0xFF`, length rounded up to 8. The ELF must be linked with `linker_flash_s32k344_app.ld`.
- Uses the raw Waveshare 20-byte serial frames (`AA 55 ...`, checksum = sum of bytes 2..18). The adapter is configured with the CAN bitrate before the first command.
- Needs **Python 3** (the Python bundled with S32 Design Studio is Python 2 and will not work: use `py -3`).
- Prints the board's answer to CONNECT: app version, boot state, result of the last bootloader copy.

---------------------------------------------------------------------------------------------------

## 7. Set-up (S32 Design Studio)

1. Make two projects from your working one: `..._BOOTLOADER` and `..._APP`.
2. Copy `common/*` into each project's `src` (or add the folder to the include path). Replace `src/main.c` with the matching `main.c`.
3. Put the matching linker script in `Project_Settings/Linker_Files` and select it (or replace `linker_flash_s32k344.ld` with its content).
4. The `C40_Ip` driver must be in both projects (`C40_Ip_Cfg.c`, `C40_Ip_Cfg.h`).
5. Build. The linker stops with a message if the bootloader is larger than 256 KB, the app larger than 1 MB, or the app's vector table is not at `0x00502000`.
6. **First programming (debugger):** flash the bootloader, then the app. Use "erase only the sectors being programmed", never "erase all", or one will wipe the other.
7. **Test the cold boot first:** stop the debug session, power-cycle the board, and check the LED (section 8). Only then use the CAN flasher.

---------------------------------------------------------------------------------------------------

## 8. LEDs and debug variables

Green LED (PTA27):

| What you see | Meaning |
|---|---|
| blinks once a second | application is running (heartbeat) |
| solid on | bootloader is copying the new image |
| N blinks, long pause, repeat | bootloader stopped, N = failure code: **1** flash driver init failed, **2** staged image invalid, **3** erase of APP failed, **4** copy / program / verify failed, **5** no valid application in flash, **6** saving the boot record failed |

Debugger variables (Live Expressions):

| Variable | Where | Meaning |
|---|---|---|
| `g_blStage` | bootloader | 1 start, 2 flash init done, 3 record read, 4 jumping, 5 copying, 6 copy done, 7 record saved, 9 stopped |
| `g_blFail` | bootloader | failure code (same numbers as the blinks) |
| `g_bfErr` | both | last flash driver error: high byte = step (1 erase start, 2 erase status, 3 write start, 4 write status, 5 verify, 6 unlock), low byte = driver status |
| `otaState`, `otaNextOffset`, `otaImageSize`, `otaLastCmd`, `otaLastStatus` | app | OTA session state (0 idle, 1 connected, 2 receiving, 3 finished) |
| `canRxLog[]`, `canRxLast`, `canRxCount` | app | last received OTA command frames |

---------------------------------------------------------------------------------------------------

## 9. Known limits and what is not verified

- Nothing here was run on the board by me. The first full OTA transfer and erase/program of STORAGE/INFO worked on your board; the **bootloader copy and the jump into the app from a cold boot are not yet confirmed**.
- If the board looks dead after an OTA, first make sure the debug session is closed and the board was power-cycled: an attached debugger can keep the core halted after the software reset.
- If the bootloader fails or the app crashes, the board cannot be re-flashed over CAN (the bootloader does not listen on CAN). Recovery is the debugger. A CAN receive window in the bootloader would remove this limit.
- There is no signature or encryption: the check is CRC16 plus the vector-table test.
- The boot ROM only starts code at `0x00400000`, so the application is always started by the bootloader. If you erase the bootloader, the app will not start on its own.
- Core clock for the app's timers is assumed to be 160 MHz (`CORE_CLK_HZ` in `app/main.c`).
