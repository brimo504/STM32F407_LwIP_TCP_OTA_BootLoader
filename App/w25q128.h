#ifndef __W25Q128_H
#define __W25Q128_H

#include "main.h"
#include <stdint.h>

/* 芯片ID定义 */
#define W25Q128_ID        0x684018
/* 第1字节：厂商ID 0x68（国产兼容版，华邦原厂为0xEF）
 * 第2字节：内存类型 0x40 标准SPI NOR Flash
 * 第3字节：容量 0x18 = 128Mbit = 16MB */
#define W25Q128_ID_MASK   0x00FFFF   /* 只比对容量+类型，放行不同厂商的兼容片 */

/* SPI Flash 指令集 */
#define W25X_WriteEnable        0x06
#define W25X_WriteDisable       0x04
#define W25X_ReadStatusReg      0x05
#define W25X_WriteStatusReg     0x01
#define W25X_ReadData           0x03
#define W25X_PageProgram        0x02
#define W25X_SectorErase        0x20
#define W25X_BlockErase32K      0x52
#define W25X_BlockErase64K      0xD8
#define W25X_ChipErase          0xC7
#define W25X_JedecID            0x9F
#define W25X_PowerDown          0xB9
#define W25X_ReleasePowerDown   0xAB

/* 容量参数 */
#define W25Q128_TOTAL_SIZE      (16 * 1024 * 1024)  /* 16MB */
#define W25Q_SECTOR_SIZE        4096                 /* 4KB/扇区 */
#define W25Q_PAGE_SIZE          256                  /* 256字节/页 */

/* 公共接口 */
int      W25Q_Init(void);
uint8_t  W25Q_IsPresent(void);
uint32_t W25Q_ReadID(void);
uint8_t  W25Q_ReadSR(void);
void     W25Q_WriteEnable(void);
void     W25Q_WaitBusy(void);
void     W25Q_Read(uint8_t *pBuf, uint32_t addr, uint16_t len);
uint16_t W25Q_WritePage(const uint8_t *pBuf, uint32_t addr, uint16_t len);
uint32_t W25Q_WriteBuffer(const uint8_t *pBuf, uint32_t addr, uint32_t len);
void     W25Q_EraseSector(uint32_t sector_addr);
void     W25Q_EraseChip(void);
void     W25Q_PowerDown(void);
void     W25Q_Wakeup(void);

/*
 * 使用注意：
 * 1. W25Q_WritePage 是"单页编程"原语，不会跨页，超过256字节会被截断并在页内回卷，
 *    写任意长度数据请统一使用 W25Q_WriteBuffer()。
 * 2. NOR Flash 只能把 bit 由 1 写成 0，写入前必须先用 W25Q_EraseSector 擦除对应扇区，
 *    W25Q_WriteBuffer 不会自动擦除。
 * 3. W25Q_EraseSector 擦的是"地址所在的那一个 4KB 扇区"，不是"从该地址开始的 4KB"。
 *    参数区双备份的两个副本必须落在不同的 4KB 扇区上（例如 0x000000 与 0x001000）。
 */
#endif
