#include "boot_param.h"
#include <string.h>

/* 硬件CRC句柄，CubeMX生成全局变量，Bootloader与APP工程均需开启CRC外设 */
extern CRC_HandleTypeDef hcrc;

/* 片内Flash总容量（F407ZGT6 = 1MB）。若换型号请在工程宏中覆盖本定义 */
#ifndef BOOT_FLASH_TOTAL_SIZE
#define BOOT_FLASH_TOTAL_SIZE   (1024U * 1024U)
#endif
#define BOOT_FLASH_END_ADDR     (FLASH_BASE + BOOT_FLASH_TOTAL_SIZE)

#if (GOLDEN_A_ADDR + APP_A_SIZE) > RUN_B_ADDR
#warning "A区范围与B区重叠：boot_flash_erase_safe() 会拒绝擦除 B 区首扇区，在线升级将不可用，请修正 APP_A_SIZE"
#endif

/**
 * @brief 喂狗钩子弱定义：默认空实现
 * @note  Bootloader/APP 工程可在自己的代码中重新实现（不要加 __WEAK），
 *        例如：void boot_wdg_refresh(void){ HAL_IWDG_Refresh(&hiwdg); }
 */
__WEAK void boot_wdg_refresh(void)
{
    /* 默认不喂狗；长循环调用处已埋点，用户实现后自动生效 */
}

/**
 * @brief 硬件CRC32通用计算（CRC32-Ethernet：多项式0x04C11DB7，初始值0xFFFFFFFF）
 * @param data 数据缓冲区首地址
 * @param len 字节长度，必须为4的整数倍
 * @return CRC32结果；输入非法返回0（注意0也是合法CRC值，判错请用boot_crc32_calc_ex）
 */
uint32_t hw_crc32_calc(const uint8_t *data, uint32_t len)
{
    if((len == 0U) || (len % 4U != 0U))
    {
        return 0U;
    }

    __HAL_CRC_DR_RESET(&hcrc);
    hcrc.Instance->DR = 0xFFFFFFFFU;

    uint32_t word_cnt = len / 4U;
    return HAL_CRC_Accumulate(&hcrc, (uint32_t *)data, word_cnt);
}

/**
 * @brief 硬件CRC32计算（带显式状态码）
 */
int boot_crc32_calc_ex(const uint8_t *data, uint32_t len, uint32_t *out_crc)
{
    if((data == NULL) || (out_crc == NULL))
    {
        return -1;
    }
    if(boot_len_is_valid(len, 0xFFFFFFFFU) == 0)
    {
        return -1;
    }

    *out_crc = hw_crc32_calc(data, len);
    return 0;
}

/**
 * @brief 计算BootPara_t结构体业务域的CRC32（跳过chk_sum字段）
 * @param p 结构体指针
 * @return CRC32结果
 * @note  参与校验的长度为 offsetof(BootPara_t, chk_sum)，必须是4的倍数，
 *        否则 hw_crc32_calc 会直接返回0，导致 chk_sum 恒为0、校验形同虚设。
 *        下面用编译期断言把这个隐患在编译阶段暴露出来。
 */
#if defined(__GNUC__) || (defined(__ARMCC_VERSION) && (__ARMCC_VERSION >= 6000000U))
_Static_assert((offsetof(BootPara_t, chk_sum) % 4U) == 0U,
               "BootPara_t: chk_sum 偏移必须4字节对齐，请检查 flag 成员类型宽度");
#endif

uint32_t boot_calc_struct_crc32(BootPara_t *p)
{
    uint8_t *buf = (uint8_t *)p;
    uint32_t calc_len = offsetof(BootPara_t, chk_sum);
    return hw_crc32_calc(buf, calc_len);
}

/**
 * @brief 【私有静态函数】读取单一份W25Q参数副本，并执行CRC32完整性校验
 * @param w25q_addr W25Q参数副本地址
 * @param out 输出读到的结构体到内存
 * @retval 0：副本有效；-1：magic错误 或 CRC32不匹配，副本损坏
 */
static int boot_read_one_para(uint32_t w25q_addr, BootPara_t *out)
{
    W25Q_Read((uint8_t *)out, w25q_addr, sizeof(BootPara_t));

    /* 校验1：魔数检查 */
    if(out->magic != BOOT_PARA_MAGIC)
    {
        return -1;
    }

    /* 校验2：业务域CRC32完整性校验 */
    uint32_t calc_crc = boot_calc_struct_crc32(out);
    if(calc_crc != out->chk_sum)
    {
        return -1;
    }

    return 0;
}

/**
 * @brief 读取W25Q双备份参数，自动选择可用副本；两份损坏输出安全默认IDLE参数
 */
int boot_read_double_para(BootPara_t *out_para)
{
    BootPara_t para1, para2;
    int ret1 = boot_read_one_para(W25Q_PARA1_ADDR, &para1);
    int ret2 = boot_read_one_para(W25Q_PARA2_ADDR, &para2);

    /* 预先填充安全默认值：全部损坏时使用空闲状态，防止跑异常升级。
     * 必须整体清零，否则 reserved[3] 会是栈上的随机值并被写回 W25Q。 */
    memset(out_para, 0, sizeof(BootPara_t));
    out_para->magic    = BOOT_PARA_MAGIC;
    out_para->flag     = BOOT_FLAG_IDLE;
    out_para->fw_crc32 = 0U;
    out_para->fw_len   = 0U;
    out_para->appB_crc = 0U;
    out_para->chk_sum  = boot_calc_struct_crc32(out_para);

    /* 优先级逻辑：两份都有效取para1；哪份有效就用哪一份 */
    if(ret1 == 0 && ret2 == 0)
    {
        *out_para = para1;
        return 0;
    }
    if(ret1 == 0)
    {
        *out_para = para1;
        return 0;
    }
    if(ret2 == 0)
    {
        *out_para = para2;
        return 0;
    }
    /* 两份副本全部损坏，已经赋值上面的安全默认 */
    return -1;
}

/**
 * @brief 将参数双备份写入W25Q；内部自动计算填充chk_sum，写入后读回比对
 */
int boot_write_double_para(BootPara_t *in_para)
{
    BootPara_t temp = *in_para;
    BootPara_t back1, back2;

    temp.magic   = BOOT_PARA_MAGIC;   /* 强制写魔数，防止调用方忘记填 */
    temp.chk_sum = boot_calc_struct_crc32(&temp);

    /* 擦写第一份参数副本 */
    W25Q_EraseSector(W25Q_PARA1_ADDR);
    if(W25Q_WriteBuffer((const uint8_t *)&temp, W25Q_PARA1_ADDR, sizeof(BootPara_t)) != sizeof(BootPara_t))
    {
        return -1;
    }

    /* 擦写第二份参数副本 */
    W25Q_EraseSector(W25Q_PARA2_ADDR);
    if(W25Q_WriteBuffer((const uint8_t *)&temp, W25Q_PARA2_ADDR, sizeof(BootPara_t)) != sizeof(BootPara_t))
    {
        return -1;
    }

    /* 写后读回比对：两份副本必须完全一致才算写入可信 */
    W25Q_Read((uint8_t *)&back1, W25Q_PARA1_ADDR, sizeof(BootPara_t));
    W25Q_Read((uint8_t *)&back2, W25Q_PARA2_ADDR, sizeof(BootPara_t));

    if(memcmp(&temp, &back1, sizeof(BootPara_t)) != 0)
    {
        return -2;
    }
    if(memcmp(&temp, &back2, sizeof(BootPara_t)) != 0)
    {
        return -3;
    }

    return 0;
}

/**
 * @brief MSP栈指针基础合法性校验
 * @note  判定落在 F407 真实可寻址的 RAM 区间内，且栈顶必须4字节对齐：
 *        常规SRAM 0x20000000~0x20020000(128KB) 或 CCM 0x10000000~0x10010000(64KB)
 */
uint8_t boot_check_msp_valid(uint32_t flash_addr)
{
    if(flash_addr < FLASH_BASE || (flash_addr + 8U) > BOOT_FLASH_END_ADDR)
    {
        return 0U;
    }

    uint32_t msp = *(volatile uint32_t *)flash_addr;

    /* 栈顶必须4字节对齐 */
    if((msp & 3U) != 0U)
    {
        return 0U;
    }

    /* 常规 SRAM（含栈顶正好等于末地址+1 的情况） */
    if((msp > BOOT_SRAM_BASE_ADDR) && (msp <= BOOT_SRAM_END_ADDR))
    {
        return 1U;
    }

    /* CCM RAM */
    if((msp > BOOT_CCM_BASE_ADDR) && (msp <= BOOT_CCM_END_ADDR))
    {
        return 1U;
    }

    return 0U;
}

/**
 * @brief 读取W25Q指定区域，计算固件CRC32
 */
uint32_t calc_w25q_fw_crc(uint32_t w25q_addr, uint32_t len)
{
    if((len == 0U) || (len % 4U != 0U))
    {
        return 0U;
    }

    /* 必须4字节对齐：HAL_CRC_Accumulate 按字访问，未对齐缓冲区在部分编译器下会硬 fault */
    __ALIGNED(4) uint8_t buf[256U];
    uint32_t offset = 0U;

    __HAL_CRC_DR_RESET(&hcrc);
    hcrc.Instance->DR = 0xFFFFFFFFU;

    uint32_t crc_val = 0xFFFFFFFFU;

    while(offset < len)
    {
        uint32_t remain = len - offset;
        uint16_t read_len = (remain >= 256U) ? 256U : (uint16_t)remain;

        W25Q_Read(buf, w25q_addr + offset, read_len);

        uint32_t word_cnt = read_len / 4U;
        crc_val = HAL_CRC_Accumulate(&hcrc, (uint32_t *)buf, word_cnt);

        offset += read_len;
        boot_wdg_refresh();   /* 长循环喂狗钩子 */
    }
    return crc_val;
}

/**
 * @brief 计算片内Flash一段区域的完整固件CRC32
 */
uint32_t boot_calc_flash_crc(uint32_t addr, uint32_t len)
{
    if((len == 0U) || (len % 4U != 0U))
    {
        return 0U;
    }
    if(addr < FLASH_BASE || (addr + len) > BOOT_FLASH_END_ADDR)
    {
        return 0U;
    }

    __HAL_CRC_DR_RESET(&hcrc);
    hcrc.Instance->DR = 0xFFFFFFFFU;

    uint32_t word_cnt = len / 4U;
    return HAL_CRC_Accumulate(&hcrc, (uint32_t *)addr, word_cnt);
}

#ifdef BOOTLOADER_BUILD

/**
 * @brief 根据Flash地址返回对应扇区编号
 * @note  F407 扇区划分：S0~S3 各16KB，S4 64KB，S5~S11 各128KB。
 *        旧实现把 >=0x08060000 的地址一律返回 S7，覆盖了 S8~S11，此处补全。
 */
static uint32_t boot_get_flash_sector(uint32_t addr)
{
    if(addr < 0x08004000U)      return FLASH_SECTOR_0;
    else if(addr < 0x08008000U) return FLASH_SECTOR_1;
    else if(addr < 0x0800C000U) return FLASH_SECTOR_2;
    else if(addr < 0x08010000U) return FLASH_SECTOR_3;
    else if(addr < 0x08020000U) return FLASH_SECTOR_4;
    else if(addr < 0x08040000U) return FLASH_SECTOR_5;
    else if(addr < 0x08060000U) return FLASH_SECTOR_6;
    else if(addr < 0x08080000U) return FLASH_SECTOR_7;
    else if(addr < 0x080A0000U) return FLASH_SECTOR_8;
    else if(addr < 0x080C0000U) return FLASH_SECTOR_9;
    else if(addr < 0x080E0000U) return FLASH_SECTOR_10;
    else                        return FLASH_SECTOR_11;
}

/**
 * @brief 安全擦除STM32片内Flash扇区；代码层硬性拦截A-Golden防砖分区
 */
int boot_flash_erase_safe(uint32_t addr)
{
    const uint32_t A_START = GOLDEN_A_ADDR;
    const uint32_t A_END   = (GOLDEN_A_ADDR + APP_A_SIZE);

    /* 地址必须在片内Flash范围内 */
    if((addr < FLASH_BASE) || (addr >= BOOT_FLASH_END_ADDR))
    {
        return -2;
    }

    /* 如果地址落在A-Golden防砖分区，直接拒绝擦除 */
    if(addr >= A_START && addr < A_END)
    {
        return -1;
    }

    HAL_FLASH_Unlock();

    FLASH_EraseInitTypeDef erase_conf = {0};
    uint32_t sector_err = 0U;
    erase_conf.TypeErase    = FLASH_TYPEERASE_SECTORS;
    erase_conf.VoltageRange = FLASH_VOLTAGE_RANGE_3;
    erase_conf.NbSectors    = 1U;
    erase_conf.Sector       = boot_get_flash_sector(addr);

    if(HAL_FLASHEx_Erase(&erase_conf, &sector_err) != HAL_OK)
    {
        HAL_FLASH_Lock();
        return -3;
    }

    HAL_FLASH_Lock();
    return 0;
}

/**
 * @brief 将W25Q SPI Flash固件镜像拷贝写入STM32片内B区Flash
 */
int boot_copy_w25q_to_internal_flash(uint32_t src_w25q_addr, uint32_t dst_flash_addr, uint32_t len)
{
    /* 参数检查：非0、4字节对齐、不超过B区容量 */
    if(boot_len_is_valid(len, APP_B_SIZE) == 0)
    {
        return -1;
    }

    /* 目标区间必须完整落在B区，禁止写 Bootloader / A-Golden 分区 */
    if((dst_flash_addr < RUN_B_ADDR) ||
       ((dst_flash_addr + len) > (RUN_B_ADDR + APP_B_SIZE)))
    {
        return -2;
    }
    if((dst_flash_addr & 3U) != 0U)
    {
        return -2;
    }

    __ALIGNED(4) uint8_t buf[256U];
    uint32_t offset = 0U;

    HAL_FLASH_Unlock();

    while(offset < len)
    {
        uint16_t copy_len = ((len - offset) >= 256U) ? 256U : (uint16_t)(len - offset);

        /* 从W25Q读取一页数据到RAM缓冲区 */
        W25Q_Read(buf, src_w25q_addr + offset, copy_len);

        /* 按4字节字写入片内Flash，必须检查每一次编程结果 */
        for(uint16_t i = 0U; i < copy_len; i += 4U)
        {
            uint32_t data = *(uint32_t *)(void *)(buf + i);
            if(HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD,
                                 dst_flash_addr + offset + i, data) != HAL_OK)
            {
                HAL_FLASH_Lock();
                return -3;
            }
        }

        offset += copy_len;
        boot_wdg_refresh();   /* 长循环喂狗钩子 */
    }

    HAL_FLASH_Lock();
    return 0;
}

#endif
