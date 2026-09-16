#include "restart.h"
#include "w25q128.h"

/*
 准备：
 (1)取APP主栈指针 MSP 初始值（栈顶地址）
 (2)取APP复位函数入口地址（APP_BASE_ADDR + 4）
 执行：
 （1）关闭bootloader开启的外设、时钟 ：HAL_RCC_DeInit();HAL_DeInit();
 （2）关闭所有使能的中断 ： __disable_irq()
 （3）设置向量表偏移指向APP主栈指针：__set_MSP() 把 APP 的栈顶写入 MSP 寄存器
 （4）程序跳转：直接 PC 寄存器赋值为 APP 复位入口，不返回
 */

 /*
 常见踩坑点：
  1.忘记 SCB->VTOR**：APP 一进中断就 HardFault          —— 本文件已修复（见 jump_to_app）
  2. 跳转前没关 Bootloader 外设 / 中断：跳过去随机死机   —— 本文件已修复
  3. 栈地址校验失败：跳转到非法地址，HardFault          —— 本文件已修复
  4. 链接脚本 FLASH 起始地址没改：APP 烧录位置不对
  5. 使用`NVIC_SystemReset()`软复位代替跳转：软复位会重新回到 Bootloader，不是 APP
  6. 跳转前开着全局中断：跳转过程触发中断，系统崩溃
  7. 跳转前只 __disable_irq() 却不开回来：APP 若没有显式 __enable_irq() 会永远无中断
     本实现在 VTOR、MSP 都切到 APP 之后再 __enable_irq()，此时来中断是安全的
 */
 /*
 知识点：
 (1)Cortex_M4 有 MSP 主栈和 PSP 进程栈: 复位默认用 MSP，所有中断异常处理强制使用 MSP；PSP 只能在线程模式使用，一般给 RTOS 任务做独立栈；
 (2)中断触发时硬件自动切 MSP，中断返回切回 PSP，实现任务栈和中断栈分离，节约 RAM;裸机和 Bootloader 全程只用 MSP。
 (3)存储器	            STM32F407	    掉电	读写特性	                  存放内容
    Flash（俗称 ROM）	1MB/2MB	      保存	可读，必须扇区擦除才能写	  Bootloader、APP 程序、const 常量
    SRAM	            192+64KB	    丢失	随机快速读写	              栈、堆、全局变量、非 const 静态变量、缓冲区

 (4)全局变量、非 const 静态变量：.data段（初始化的全局变量）、.bss段（未初始化全局变量，启动代码清零）
 (5)
    运行时临时缓存：串口接收缓冲区、DMA 缓存、升级固件临时缓冲区
    程序运行时：CPU 从 Flash 读取指令，在 SRAM 里面读写变量、堆栈。
    Flash 只负责存放代码和常量；所有动态读写都必须在 SRAM
 (6)
 STM32上电工作流程：
 -上电后，Cortex-M4 内核硬件(1)自动读取 Flash 向量表(2)取出 MSP 栈顶，写入内核 MSP 寄存器（3）读取Reset_Handler 复位入口地址，赋值给 PC，跳转到复位汇编函数
 -执行启动文件汇编代码：
 （1）Reset_Handler 调用 SystemInit()，配置系统时钟（HSE/PLL）
 （2）进入 __main（__main 是 ARM 编译器（ARMCC）内置库入口函数，负责内存段搬运和清零，在启动文件 `startup_stm32f407xx.s` 里调用)
      把 Flash 里存放的.data 段（已初始化全局变量）拷贝到 SRAM
      把 SRAM 中的 .bss 段（未初始化全局变量）清零
 -调用用户的 main():初始化 HAL 库、SysTick、初始化外设
 (7)
 Keil 下载时，默认擦除工程链接脚本指定 FLASH 起始地址对应的扇区
 */
 /*
# Bootloader 工程配置（文本版，STM32F407，Keil MDK ARMCC）
> 分区：Bootloader 0x08000000 ~ 0x0800FFFF（64KB）
> 头文件宏：

#define APP_BASE_ADDR 0x08000000
#define APP_A_OFFSET  0x10000
#define APP_B_OFFSET  0x60000   // 修改为扇区边界0x08060000

## 1. Options for Target → Target

Read_only Memory(ROM1)
- Start：0x08000000
- Size：0x00010000

Read_write Memory(RAM1)
- Start：`0x20000000`
- Size：`0x00030000`

## 2. Options for Target → Debug

Use：ST?Link Debugger
点击【Settings】→ Flash Download

- Erase：`Erase Sectors`
- Programming：? Program  ? Verify  ? Reset and Run
- Flash Programming Algorithm → Add：`0x08000000 ~ 0x08010000`

## 3. Options for Target → Utilities

Use Target Driver for Flash Programming：ST?Link Debugger
点击【Settings】→ Flash Download

- Erase：`Erase Sectors`
- Programming：? Program  ? Verify  ? Reset and Run
- Flash Programming Algorithm：`0x08000000 ~ 0x08010000`

## 4. Options for Target → Output

- ? Create HEX File
- Name of Executable：bootloader
- 生成 bin 文件：打开【User】选项卡，Run #1
  [fromelf.exe的路径] --bin --output [输出文件路径] [输入文件路径]
  @P：代表当前工程文件（.uvprojx）所在的目录。
  @L：代表输出文件的基本名称（即“Name of Executable”中设置的名字）
  fromelf --bin --output .\Farmware\farmware.bin @P\@L.axf

 */

/* 跳转前对 APP 复位入口的地址范围检查上限（F407ZGT6 = 1MB） */
#ifndef BOOT_APP_FLASH_SIZE
#define BOOT_APP_FLASH_SIZE   (1024U * 1024U)
#endif

typedef void (* pFunction)(void);

/**
 * @brief  跳转到 A-Golden(0) 或 B-Run(1) 镜像
 * @param  i 0=A区防砖镜像，1=B区可升级镜像
 * @retval 0 不应返回（成功跳转）；-1 参数/镜像非法，未跳转
 * @note   调用方必须对返回值做兜底（while(1) 或再次尝试另一个分区），
 *         不能假设"调用了就一定会跳走"。
 */
int jump_to_app(uint8_t i)
{
    uint32_t app_addr;
    uint32_t app_msp;
    uint32_t app_reset_entry;

    if(i == 0U)
    {
        app_addr = APP_BASE_ADDR + APP_A_OFFSET;
    }
    else
    {
        app_addr = APP_BASE_ADDR + APP_B_OFFSET;
    }

    /* 向量表基址必须 128 字节对齐（VTOR 低 7 位是保留位） */
    if((app_addr & 0x7FU) != 0U)
    {
        return -1;
    }

    /* MSP 合法性：复用 boot_param 的区间判定（含 SRAM / CCM / 4字节对齐） */
    if(boot_check_msp_valid(app_addr) == 0U)
    {
        return -1;
    }

    app_msp         = *(volatile uint32_t *)app_addr;
    app_reset_entry = *(volatile uint32_t *)(app_addr + 4U);

    /* 复位入口必须落在片内 Flash 范围内 */
    if((app_reset_entry < FLASH_BASE) ||
       (app_reset_entry >= (FLASH_BASE + BOOT_APP_FLASH_SIZE)))
    {
        return -1;
    }
    /* 向量表里的复位入口是 Thumb 地址，bit0 必须为 1，否则跳过去直接用法错误 */
    if((app_reset_entry & 0x1U) == 0U)
    {
        return -1;
    }

    /* ---------- 以下进入不可逆区，不能再 return ---------- */

    /* 1. 复位外设与时钟，让 APP 拿到一个干净的硬件环境 */
    HAL_RCC_DeInit();
    HAL_DeInit();
    __disable_irq();

    /* 2. 关闭 SysTick：Bootloader 用它做 HAL 时基，跳过去后会指向错误向量 */
    SysTick->CTRL = 0U;
    SysTick->LOAD = 0U;
    SysTick->VAL  = 0U;

    /* 3. 关掉所有外设中断并清掉残留挂起位，避免 APP 一开中断就进旧 ISR */
    for(uint8_t n = 0U; n < 8U; n++)
    {
        NVIC->ICER[n] = 0xFFFFFFFFU;
        NVIC->ICPR[n] = 0xFFFFFFFFU;
    }
    SCB->ICSR |= SCB_ICSR_PENDSTCLR_Msk;   /* 清 SysTick 挂起 */

    __DSB();
    __ISB();

    /* 4. 关键：把向量表切到 APP。漏掉这步 = APP 一进中断就 HardFault */
    SCB->VTOR = app_addr;
    __DSB();

    /* 5. 切回特权级 + 主栈，再装载 APP 栈顶 */
    __set_CONTROL(0x0U);
    __set_MSP(app_msp);
    __DSB();
    __ISB();

    /* 6. VTOR 与 MSP 都已经指向 APP，此刻开中断是安全的。
     *    这里必须开：否则 APP 若没有显式 __enable_irq() 会永远收不到任何中断。 */
    __enable_irq();

    pFunction app_entry = (pFunction)app_reset_entry;
    app_entry();

    return 0;   /* 理论上执行不到 */
}
