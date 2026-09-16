#ifndef __BOOT_RECOVER_H
#define __BOOT_RECOVER_H

#include "main.h"
#include "boot_param.h"

/**
 * @brief  A、B 镜像全部损坏时的串口救砖通道（简易自定义协议，UART1 115200 8N1）
 *
 * 协议（上位机主动，MCU 应答）：
 *   1) MCU 每 500ms 发一个 'C'，等待握手；
 *   2) 上位机发 4 字节同步头：7E A5 5A 5A → MCU 回 "OK"；
 *   3) 上位机发 4 字节固件长度（小端），必须非0、≤APP_B_SIZE、4字节对齐
 *      → MCU 回 "OK" 或 "ER"；
 *   4) 上位机发 len 字节固件数据 → MCU 写入 W25Q 固件缓存（自动擦除扇区）；
 *   5) 上位机发 4 字节 CRC32（小端，与硬件CRC32-Ethernet一致）→ MCU 回 "OK"/"ER"；
 *   6) 校验通过后 MCU 置 flag=UPDATE_READY 并 NVIC_SystemReset()，
 *      由 Bootloader 正常走升级链把固件写进 B 区。
 *
 * @retval 成功时不会返回（直接软复位）；-1 失败/超时，调用方可重试
 * @note   若不需要该通道，把 main.c 中的调用换回 while(1) 即可，不新增任何依赖。
 */
int boot_recover_uart(void);

#endif
