#include "w25q128.h"

/* SPI 句柄（CubeMX 生成） */
extern SPI_HandleTypeDef hspi1;

/* 片选控制 */
#define FLASH_CS_LOW()   HAL_GPIO_WritePin(FLASH_CS_GPIO_Port, FLASH_CS_Pin, GPIO_PIN_RESET)
#define FLASH_CS_HIGH()  HAL_GPIO_WritePin(FLASH_CS_GPIO_Port, FLASH_CS_Pin, GPIO_PIN_SET)

/* 状态寄存器 BUSY 位轮询超时（ms） */
#define W25Q_BUSY_TIMEOUT_MS     1000

/**
 * @brief  SPI 单字节阻塞读写
 */
static uint8_t SPI_ReadWriteByte(uint8_t data)
{
    uint8_t rx_data = 0xFF;
    HAL_SPI_TransmitReceive(&hspi1, &data, &rx_data, 1, 100);
    return rx_data;
}

int W25Q_Init(void)
{
    FLASH_CS_HIGH();
    W25Q_WaitBusy();

    /* 上电后必须确认器件在位，否则后面所有读写都会静默失败 */
    if(W25Q_IsPresent() == 0U)
    {
        return -1;
    }
    return 0;
}

uint8_t W25Q_IsPresent(void)
{
    uint32_t id = W25Q_ReadID();

    /* 全部 0x00（没焊/掉电）或全部 0xFF（片选没连上）都算不在位 */
    if((id == 0x000000U) || (id == 0xFFFFFFU))
    {
        return 0U;
    }
    /* 只比对"存储类型+容量"，放行不同厂商的兼容片 */
    if((id & W25Q128_ID_MASK) != (W25Q128_ID & W25Q128_ID_MASK))
    {
        return 0U;
    }
    return 1U;
}

uint32_t W25Q_ReadID(void)
{
    uint32_t id = 0;
    FLASH_CS_LOW();
    SPI_ReadWriteByte(W25X_JedecID);
    id |= (uint32_t)SPI_ReadWriteByte(0xFF) << 16;
    id |= (uint32_t)SPI_ReadWriteByte(0xFF) << 8;
    id |= SPI_ReadWriteByte(0xFF);
    FLASH_CS_HIGH();
    return id;
}

uint8_t W25Q_ReadSR(void)
{
    uint8_t sr;
    FLASH_CS_LOW();
    SPI_ReadWriteByte(W25X_ReadStatusReg);
    sr = SPI_ReadWriteByte(0xFF);
    FLASH_CS_HIGH();
    return sr;
}

void W25Q_WriteEnable(void)
{
    FLASH_CS_LOW();
    SPI_ReadWriteByte(W25X_WriteEnable);
    FLASH_CS_HIGH();
}

/**
 * @brief  等待芯片空闲（轮询 BUSY 位，带真实时间超时）
 */
void W25Q_WaitBusy(void)
{
    uint32_t start = HAL_GetTick();
    while (((W25Q_ReadSR() & 0x01) == 0x01) &&
           ((HAL_GetTick() - start) < W25Q_BUSY_TIMEOUT_MS)) {
        /* 空转等待 */
    }
}

void W25Q_Read(uint8_t *pBuf, uint32_t addr, uint16_t len)
{
    if((pBuf == NULL) || (len == 0U))
    {
        return;
    }

    W25Q_WaitBusy();

    FLASH_CS_LOW();
    SPI_ReadWriteByte(W25X_ReadData);
    SPI_ReadWriteByte((uint8_t)(addr >> 16));
    SPI_ReadWriteByte((uint8_t)(addr >> 8));
    SPI_ReadWriteByte((uint8_t)addr);

    for (uint16_t i = 0; i < len; i++) {
        pBuf[i] = SPI_ReadWriteByte(0xFF);
    }
    FLASH_CS_HIGH();
}

/**
 * @brief  页编程原语：单次最多写一页(256B)，且不跨页
 * @return 实际写入字节数
 * @warning 不会自动跨页，超过页尾的部分会被丢弃（器件会在页内回卷）。
 *          写任意长度数据请用 W25Q_WriteBuffer()。
 */
uint16_t W25Q_WritePage(const uint8_t *pBuf, uint32_t addr, uint16_t len)
{
    if ((pBuf == NULL) || (len == 0U))
    {
        return 0U;
    }
    if (len > W25Q_PAGE_SIZE) len = W25Q_PAGE_SIZE;
    /* 页内回卷保护：不允许一次写跨越页边界 */
    if (((addr % W25Q_PAGE_SIZE) + len) > W25Q_PAGE_SIZE)
    {
        len = (uint16_t)(W25Q_PAGE_SIZE - (addr % W25Q_PAGE_SIZE));
    }

    W25Q_WaitBusy();
    W25Q_WriteEnable();
    FLASH_CS_LOW();
    SPI_ReadWriteByte(W25X_PageProgram);
    SPI_ReadWriteByte((uint8_t)(addr >> 16));
    SPI_ReadWriteByte((uint8_t)(addr >> 8));
    SPI_ReadWriteByte((uint8_t)addr);

    for (uint16_t i = 0; i < len; i++) {
        SPI_ReadWriteByte(pBuf[i]);
    }
    FLASH_CS_HIGH();
    W25Q_WaitBusy();
    return len;
}

/**
 * @brief  跨页安全写入：自动按 256B 页边界切分，可写任意长度
 * @return 实际写入字节数；与 len 不相等表示中途失败
 * @note   调用前必须保证目标扇区已擦除
 */
uint32_t W25Q_WriteBuffer(const uint8_t *pBuf, uint32_t addr, uint32_t len)
{
    if((pBuf == NULL) || (len == 0U))
    {
        return 0U;
    }
    if(((uint64_t)addr + (uint64_t)len) > (uint64_t)W25Q128_TOTAL_SIZE)
    {
        return 0U;
    }

    uint32_t written = 0U;
    while(written < len)
    {
        uint32_t page_off  = (addr + written) % W25Q_PAGE_SIZE;
        uint32_t page_left = W25Q_PAGE_SIZE - page_off;
        uint32_t chunk     = (len - written);

        if(chunk > page_left)
        {
            chunk = page_left;
        }

        uint16_t n = W25Q_WritePage(pBuf + written, addr + written, (uint16_t)chunk);
        if(n != (uint16_t)chunk)
        {
            break;
        }
        written += n;
    }
    return written;
}

void W25Q_EraseSector(uint32_t sector_addr)
{
    W25Q_WaitBusy();
    W25Q_WriteEnable();
    FLASH_CS_LOW();
    SPI_ReadWriteByte(W25X_SectorErase);
    SPI_ReadWriteByte((uint8_t)(sector_addr >> 16));
    SPI_ReadWriteByte((uint8_t)(sector_addr >> 8));
    SPI_ReadWriteByte((uint8_t)sector_addr);
    FLASH_CS_HIGH();
    W25Q_WaitBusy();
}

void W25Q_EraseChip(void)
{
    W25Q_WaitBusy();
    W25Q_WriteEnable();
    FLASH_CS_LOW();
    SPI_ReadWriteByte(W25X_ChipErase);
    FLASH_CS_HIGH();
    W25Q_WaitBusy();
}

void W25Q_PowerDown(void)
{
    FLASH_CS_LOW();
    SPI_ReadWriteByte(W25X_PowerDown);
    FLASH_CS_HIGH();
    HAL_Delay(1);
}

void W25Q_Wakeup(void)
{
    FLASH_CS_LOW();
    SPI_ReadWriteByte(W25X_ReleasePowerDown);
    FLASH_CS_HIGH();
    HAL_Delay(1);
}
