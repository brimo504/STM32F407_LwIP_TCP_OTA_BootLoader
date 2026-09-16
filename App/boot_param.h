#ifndef __BOOT_PARAM_H
#define __BOOT_PARAM_H

#include "main.h"
#include "w25q128.h"
#include <stdint.h>
#include <stddef.h>

/*
 * 工程宏配置规则（重要）：
 * 1. Bootloader 工程：Keil → Options for Target → C/C++ → Define 中添加 BOOTLOADER_BUILD；
 * 2. A-Golden / B-Run APP 工程：不要定义，APP 就看不到片内 Flash 擦写、拷贝类危险 API。
 *
 * 下面这一行是为兼容"尚未配置工程宏"的旧工程而保留的默认开关。
 * 正式量产建议删除本行，改由编译器宏提供；否则 APP 工程包含本头文件时同样会暴露
 * boot_flash_erase_safe() / boot_copy_w25q_to_internal_flash()。
 */
#define BOOTLOADER_BUILD

/*==================== Flash分区宏定义 ====================*/
#define APP_BASE_ADDR      0x08000000U
#define BOOTLOADER_ADDR    0x08000000U
/* Bootloader 自身占用：S0~S3，64KB */
#define BOOTLOADER_SIZE    (64U * 1024U)

/* A区 Golden防砖镜像：0x08010000 S4~S5，出厂固化，禁止在线擦写 */
#define GOLDEN_A_OFFSET    0x10000U
#define GOLDEN_A_ADDR      (APP_BASE_ADDR + GOLDEN_A_OFFSET)
#define APP_A_SIZE         (192U * 1024U)

/* B区 Run可更新镜像：0x08040000 S6~S7 256KB，升级重写 */
#define RUN_B_OFFSET       0x40000U
#define RUN_B_ADDR         (APP_BASE_ADDR + RUN_B_OFFSET)
#define APP_B_SIZE         (256U * 1024U)

/* B区两个扇区起始地址，避免 main.c 里硬编码 0x08040000 / 0x08060000 */
#define RUN_B_SECTOR1_ADDR  (RUN_B_ADDR)                /* S6 0x08040000 */
#define RUN_B_SECTOR2_ADDR  (RUN_B_ADDR + 0x20000U)     /* S7 0x08060000 */

/*==================== W25Q128 SPI Flash地址分配 ====================*/
#define W25Q_PARA1_ADDR    0x000000U   /* 参数备份副本1，4K扇区 */
#define W25Q_PARA2_ADDR    0x001000U   /* 参数备份副本2，4K扇区 */
#define W25Q_FW_BUF_ADDR   0x010000U   /* 待升级固件缓存起始地址 */

/*==================== 地址合法性判定宏 ====================*/
/*
 * STM32F407 常规 SRAM = SRAM1(112KB) + SRAM2(16KB)，连续映射在 0x20000000，共 128KB。
 * 注意：不是 192KB！另外 64KB 是 CCM RAM，独立映射到 0x10000000。
 * 旧代码用 0x20030000 做上限，会把 0x20020000~0x20030000 这段不存在的地址误判为合法栈顶。
 */
#define BOOT_SRAM_BASE_ADDR   0x20000000U
#define BOOT_SRAM_SIZE        (128U * 1024U)
#define BOOT_SRAM_END_ADDR    (BOOT_SRAM_BASE_ADDR + BOOT_SRAM_SIZE)

/* CCM RAM 64KB：部分工程会把栈放到 CCM，校验时需要一并接受 */
#define BOOT_CCM_BASE_ADDR    0x10000000U
#define BOOT_CCM_SIZE         (64U * 1024U)
#define BOOT_CCM_END_ADDR     (BOOT_CCM_BASE_ADDR + BOOT_CCM_SIZE)

/*==================== 升级状态枚举 ====================*/
typedef enum
{
    BOOT_FLAG_IDLE         = 0x00U,   /* 空闲，正常启动 */
    BOOT_FLAG_UPDATE_READY = 0xA5U    /* W25Q已有完整固件，等待Bootloader写入B区 */
} BootFlag_t;

/*==================== 升级参数结构体 ====================*/
/**
 * @brief 升级参数，存储于W25Q，双副本备份
 * @note reserved保留字节用于4字节对齐，适配硬件CRC；chk_sum为CRC32校验值
 * @note 业务层禁止手动赋值chk_sum，由读写API内部自动计算填充
 */
typedef struct __attribute__((packed))
{
    uint32_t    magic;          /* 魔数 BOOT_PARA_MAGIC，标记结构体有效 */
    /*
     * 这里必须用定宽 uint8_t，不能用 BootFlag_t：
     * enum 的宽度由编译器决定（AC5 默认取最小宽度=1字节，AC6/clang 默认=4字节）。
     * 若 flag 占4字节，offsetof(chk_sum) 就变成 23，非4的倍数，
     * hw_crc32_calc() 会直接返回0，导致 chk_sum 恒为0、参数校验形同虚设。
     */
    uint8_t     flag;           /* 升级状态，取值见 BootFlag_t */
    uint8_t     reserved[3];    /* 保留字节，凑4字节对齐，保证业务域长度为4的倍数 */
    uint32_t    fw_crc32;       /* W25Q缓存中待升级固件CRC32 */
    uint32_t    fw_len;         /* W25Q缓存中待升级固件实际字节长度 */
    uint32_t    appB_crc;       /* 片内B区镜像CRC32，仅Bootloader维护 */
    uint32_t    chk_sum;        /* 【内部私有】结构体业务域CRC32校验值 */
} BootPara_t;

#define BOOT_PARA_MAGIC     0xA5A5A5A5U

/*==================== 公共工具 ====================*/

/**
 * @brief 长度合法性判定：非0、不超上限、4字节对齐
 * @note  STM32F4 的 CRC 外设只有 32 位数据寄存器 CRC_DR，一次运算吃一个字。HAL 的 HAL_CRC_Accumulate(&hcrc, (uint32_t*)buf, word_cnt) 也是按 uint32_t* 逐字喂的
 * boot_len_align4() 就是给上位机/APP 侧用的补零工具，APP 写参数前应先 para.fw_len = boot_len_align4(real_len);，否则 Bootloader 会在第一道闸门就把它打回回滚 A 区
 */
__STATIC_INLINE int boot_len_is_valid(uint32_t len, uint32_t max_len)
{
    return ((len != 0U) && (len <= max_len) && ((len & 3U) == 0U)) ? 1 : 0;
}

/**
 * @brief 长度向上对齐到4字节，供上位机打包脚本 / APP侧填充固件长度使用
 */
__STATIC_INLINE uint32_t boot_len_align4(uint32_t len)
{
    return (len + 3U) & ~((uint32_t)3U);
}

/**
 * @brief 看门狗喂狗钩子（弱定义，默认空实现）
 * @note  固件校验和 Flash 拷贝都是秒级长循环。Bootloader/APP 工程可在任意位置
 *        重新实现本函数（例如 HAL_IWDG_Refresh(&hiwdg)）即可完成喂狗，
 *        本模块不直接依赖具体的看门狗句柄。
 */
void boot_wdg_refresh(void);

/*==================== 公共API (Bootloader / APP工程均可调用) ====================*/

/**
 * @brief 硬件CRC32通用计算（CRC32-Ethernet标准）
 * @param data 数据缓冲区首地址
 * @param len 字节长度，必须为4的整数倍
 * @return CRC32结果
 * @warning 输入非法时返回0，而0也是合法CRC值。判错请用 boot_crc32_calc_ex()，
 *          或在调用前用 boot_len_is_valid() 校验长度，不要拿返回值0当错误码。
 */
uint32_t hw_crc32_calc(const uint8_t *data, uint32_t len);

/**
 * @brief 硬件CRC32计算（带显式状态码，推荐替代 hw_crc32_calc）
 * @param data 数据缓冲区首地址
 * @param len 字节长度
 * @param out_crc [out] CRC32结果
 * @retval 0 成功；-1 参数非法（空指针 / 长度非0但不对齐）
 */
int boot_crc32_calc_ex(const uint8_t *data, uint32_t len, uint32_t *out_crc);

/**
 * @brief 计算BootPara_t结构体业务域的CRC32（不包含chk_sum自身）
 * @param p 结构体指针
 * @return CRC32结果
 */
uint32_t boot_calc_struct_crc32(BootPara_t *p);

/**
 * @brief 读取W25Q双备份参数，自动选择有效副本；两份损坏返回安全默认IDLE参数
 * @param out_para [out]输出参数结构体
 * @retval 0：至少一份副本有效；-1：两份副本全部损坏(输出默认安全参数)
 */
int boot_read_double_para(BootPara_t *out_para);

/**
 * @brief 将参数双备份写入W25Q；内部自动计算填充chk_sum，并在写入后读回比对
 * @param in_para 输入业务参数结构体（magic 由本函数强制填充）
 * @retval 0 成功且两份副本读回一致；
 *         -1 副本1写入长度不足；-2 副本1读回比对失败；-3 副本2读回比对失败
 * @note  返回非0仅代表本次写入不可信，由调用方决定重试还是仅打印告警
 */
int boot_write_double_para(BootPara_t *in_para);

/**
 * @brief 校验镜像向量表MSP栈指针基础合法性
 * @param flash_addr 镜像向量表起始Flash地址
 * @return 1 MSP合法；0 MSP非法
 * @note  判定区间：常规SRAM 0x20000000~0x20020000 或 CCM 0x10000000~0x10010000，
 *        且栈顶必须4字节对齐。仅基础检查，必须配合固件完整CRC32校验。
 */
uint8_t boot_check_msp_valid(uint32_t flash_addr);

/**
 * @brief 读取W25Q指定区域，计算固件CRC32
 * @param w25q_addr W25Q起始地址
 * @param len 固件字节长度，必须4字节对齐
 * @return CRC32结果，0代表输入异常
 */
uint32_t calc_w25q_fw_crc(uint32_t w25q_addr, uint32_t len);

/**
 * @brief 计算片内Flash一段区域的完整固件CRC32
 * @param addr 片内Flash起始地址
 * @param len 计算字节长度，必须4字节对齐
 * @return CRC32结果，0代表输入异常
 */
uint32_t boot_calc_flash_crc(uint32_t addr, uint32_t len);

/*==================== 仅Bootloader工程编译的危险接口 ====================*/
#ifdef BOOTLOADER_BUILD

/**
 * @brief 安全擦除片内Flash扇区；代码硬性拦截A区Golden防砖分区，禁止误擦除
 * @param addr 待擦除扇区起始地址
 * @retval 0成功；-1禁止擦A区；-2地址不在片内Flash范围；-3 HAL Flash硬件错误
 */
int boot_flash_erase_safe(uint32_t addr);

/**
 * @brief 从W25Q SPI Flash拷贝固件镜像到STM32片内Flash(B区)
 * @param src_w25q_addr W25Q源地址
 * @param dst_flash_addr 片内Flash目的地址(RUN_B_ADDR)
 * @param len 拷贝字节数，必须4字节对齐且不超过APP_B_SIZE
 * @retval 0成功；-1参数越界；-2目标区间不允许写；-3 Flash编程失败
 * @note 循环内部已调用 boot_wdg_refresh() 喂狗
 */
int boot_copy_w25q_to_internal_flash(uint32_t src_w25q_addr, uint32_t dst_flash_addr, uint32_t len);

#endif

#endif

/*
 * ================= 文件说明与业务流程解析 =================
 * 工程宏配置规则：
 * 1. Bootloader工程：Keil预处理器添加宏 BOOTLOADER_BUILD；
 * 2. A-Golden / B-Run APP工程：不定义 BOOTLOADER_BUILD；
 *    APP工程看不到片内Flash擦写、拷贝危险API，避免误改写STM32片内Flash。
 *
 * --------系统升级业务流程--------
 * 出厂状态：
 * Bootloader(0x08000000)；A-Golden(V1.0 0x08010000)；B-Run(V1.0 0x08040000)
 * W25Q双备份参数flag=BOOT_FLAG_IDLE。Bootloader上电优先校验B区，合法跳转B运行。
 *
 * 升级流程(B区V1.0→V2.0):
 * 1) B区APP接收上位机V2.0固件，分片写入W25Q_FW_BUF_ADDR固件缓存；
 * 2) APP填充BootPara_t业务字段：magic有效，flag置BOOT_FLAG_UPDATE_READY，填入fw_crc32/fw_len；
 *    （fw_len 必须用 boot_len_align4() 补到4字节对齐，否则全流程CRC都会返回0）
 * 3) APP调用boot_write_double_para()写入W25Q双备份；执行NVIC_SystemReset()软件复位；
 * 4) 重启进入Bootloader，boot_read_double_para读取参数；检测flag==BOOT_FLAG_UPDATE_READY；
 * 5) Bootloader调用calc_w25q_fw_crc校验W25Q缓存固件CRC；校验失败清除标记直接回滚A区；
 * 6) CRC通过：调用boot_flash_erase_safe擦B区S6、S7两个扇区；
 *    boot_copy_w25q_to_internal_flash拷贝W25Q固件至片内B区；
 * 7) 拷贝完成，调用boot_calc_flash_crc计算B区完整CRC32；
 *    更新para.appB_crc，flag改为BOOT_FLAG_IDLE；双备份写回W25Q；
 * 8) Bootloader校验B区镜像(MSP+完整固件CRC32)；合法则跳转B区运行V2.0；
 *
 * 故障分支：升级写B区中途断电
 * 重新上电Bootloader读到UPDATE_READY标记；再次校验W25Q固件，尝试重写B区；
 * 失败则置IDLE；B区校验失败，直接跳转A区回滚，设备可用。
 *
 * 关键约束：
 * ①A-Golden分区任何在线升级流程禁止擦写，boot_flash_erase_safe做代码层防护拦截；
 * ②回滚策略：B异常直接跳转A运行，不做镜像拷贝；
 * ③全链路统一硬件CRC32校验，算法唯一，便于上位机对齐；
 * ④固件长度必须4字节对齐（boot_len_align4），CRC接口拒绝非对齐输入；
 * ⑤所有CRC接口的返回值0同时表示"输入非法"，判错前必须先用boot_len_is_valid()。
 */
