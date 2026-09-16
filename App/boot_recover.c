#include "boot_recover.h"
#include "w25q128.h"
#include "restart.h"
#include <string.h>

/* 串口句柄（CubeMX 生成，与 printf 共用 UART1） */
extern UART_HandleTypeDef huart1;

#define RECOV_SYNC0              0x7EU
#define RECOV_SYNC1              0xA5U
#define RECOV_SYNC2              0x5AU
#define RECOV_SYNC3              0x5AU

#define RECOV_HANDSHAKE_TIMEOUT  60000U   /* 等待上位机握手总时长 ms */
#define RECOV_HANDSHAKE_PERIOD   500U     /* 'C' 发送间隔 ms */
#define RECOV_FIELD_TIMEOUT      3000U    /* 长度/CRC 字段超时 ms */
#define RECOV_DATA_TIMEOUT       3000U    /* 每个数据块超时 ms */
#define RECOV_CHUNK              256U     /* 单块大小，栈占用可控 */

static void recov_send(const char *s)
{
    HAL_UART_Transmit(&huart1, (uint8_t *)s, (uint16_t)strlen(s), 200U);
}

static int recov_recv(uint8_t *buf, uint32_t len, uint32_t timeout)
{
    if(buf == NULL || len == 0U || len > 0xFFFFU)
    {
        return 0;
    }
    return (HAL_UART_Receive(&huart1, buf, (uint16_t)len, timeout) == HAL_OK) ? 1 : 0;
}

static uint32_t recov_u32_le(const uint8_t *p)
{
    return ((uint32_t)p[0]) |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

int boot_recover_uart(void)
{
    uint8_t  buf[RECOV_CHUNK];
    uint8_t  field[4];
    uint32_t fw_len = 0U;
    uint32_t crc_host, crc_calc;
    uint32_t offset, sec, sec_cnt;
    uint32_t start;
    int      handshaked = 0;

    /* ---------- 1. 握手：周期发 'C'，等待同步头 ---------- */
    start = HAL_GetTick();
    while((HAL_GetTick() - start) < RECOV_HANDSHAKE_TIMEOUT)
    {
        recov_send("C");

        if(HAL_UART_Receive(&huart1, &field[0], 1U, RECOV_HANDSHAKE_PERIOD) != HAL_OK)
        {
            continue;
        }
        if(field[0] != RECOV_SYNC0)
        {
            continue;
        }
        if(recov_recv(&field[1], 3U, RECOV_FIELD_TIMEOUT) == 0)
        {
            break;
        }
        if((field[1] == RECOV_SYNC1) && (field[2] == RECOV_SYNC2) && (field[3] == RECOV_SYNC3))
        {
            recov_send("OK");
            handshaked = 1;
            break;
        }
    }

    if(handshaked == 0)
    {
        return -1;
    }

    /* ---------- 2. 收固件长度 ---------- */
    if(recov_recv(field, 4U, RECOV_FIELD_TIMEOUT) == 0)
    {
        return -1;
    }
    fw_len = recov_u32_le(field);
    if(boot_len_is_valid(fw_len, APP_B_SIZE) == 0)
    {
        recov_send("ER");
        return -1;
    }
    recov_send("OK");

    /* ---------- 3. 擦除固件缓存扇区 ---------- */
    sec_cnt = (fw_len + W25Q_SECTOR_SIZE - 1U) / W25Q_SECTOR_SIZE;
    for(sec = 0U; sec < sec_cnt; sec++)
    {
        W25Q_EraseSector(W25Q_FW_BUF_ADDR + sec * W25Q_SECTOR_SIZE);
        boot_wdg_refresh();
    }

    /* ---------- 4. 收固件数据并写入 W25Q ---------- */
    offset = 0U;
    while(offset < fw_len)
    {
        uint32_t chunk = fw_len - offset;
        if(chunk > RECOV_CHUNK)
        {
            chunk = RECOV_CHUNK;
        }

        if(recov_recv(buf, chunk, RECOV_DATA_TIMEOUT) == 0)
        {
            recov_send("ER");
            return -1;
        }
        if(W25Q_WriteBuffer(buf, W25Q_FW_BUF_ADDR + offset, chunk) != chunk)
        {
            recov_send("ER");
            return -1;
        }

        offset += chunk;
        boot_wdg_refresh();
    }

    /* ---------- 5. 收 CRC32 并校验 ---------- */
    if(recov_recv(field, 4U, RECOV_FIELD_TIMEOUT) == 0)
    {
        return -1;
    }
    crc_host = recov_u32_le(field);
    crc_calc = calc_w25q_fw_crc(W25Q_FW_BUF_ADDR, fw_len);

    if((crc_calc == 0U) || (crc_calc != crc_host))
    {
        recov_send("ER");
        return -1;
    }
    recov_send("OK");

    /* ---------- 6. 置升级标记并软复位，交给 Bootloader 升级链写入 B 区 ---------- */
    {
        BootPara_t para;

        (void)boot_read_double_para(&para);
        para.magic    = BOOT_PARA_MAGIC;
        para.fw_crc32 = crc_host;
        para.fw_len   = fw_len;
        para.flag     = BOOT_FLAG_UPDATE_READY;

        if(boot_write_double_para(&para) != 0)
        {
            recov_send("ER");
            return -1;
        }
    }

    recov_send("DONE");
    HAL_Delay(20U);
    NVIC_SystemReset();

    while(1)
    {
        /* 不应执行到这里 */
    }
}
