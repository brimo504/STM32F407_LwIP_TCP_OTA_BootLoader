/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "crc.h"
#include "spi.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h>
#include <stdarg.h>
#include "restart.h"
#include "boot_param.h"
#include "boot_recover.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
/* printf 重定向到 USART1。注意：必须关闭 semihosting（Keil 勾选 Use MicroLIB，
   或在工程中加入 #pragma import(__use_no_semihosting)），否则程序会卡在半主机调用。 */
int fputc(int ch, FILE *f)
{
    HAL_UART_Transmit(&huart1, (uint8_t*)&ch,1,5);
    return ch;
}

/**
 * @brief 喂狗钩子实现：固件校验/Flash拷贝等长循环会自动调用到这里
 * @note  若 Bootloader 开启了独立看门狗，取消下面一行注释即可
 */
void boot_wdg_refresh(void)
{
    /* HAL_IWDG_Refresh(&hiwdg); */
}

/**
 * @brief 跳转兜底：jump_to_app 成功时不会返回，返回说明目标镜像非法。
 *        旧代码直接忽略返回值，跳转失败后程序会继续往下跑，状态机错乱。
 */
static void boot_jump_or_halt(uint8_t idx)
{
    if(jump_to_app(idx) != 0)
    {
        printf("Boot: jump to %s failed, halt\r\n", (idx == 0U) ? "A" : "B");
    }
    while(1)
    {
        /* 跳转失败，停在这里等看门狗或人工干预 */
    }
}
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_USART1_UART_Init();
  MX_SPI1_Init();
  MX_CRC_Init();
  /* USER CODE BEGIN 2 */
  if(W25Q_Init() != 0)
  {
      printf("Boot: WARN W25Q128 not present, update & para unavailable\r\n");
  }
  printf("\r\n==== Bootloader Start ====\r\n");

  BootPara_t para;
  // 读取W25Q双备份参数，双份损坏自动返回安全默认IDLE参数
  // 必须检查返回值：两份都损坏时 fw_len=0，后续不能拿它去做CRC校验
  if(boot_read_double_para(&para) != 0)
  {
      printf("Boot: both para copies invalid, use safe default(IDLE)\r\n");
  }

  /************************* 【升级分支】检测待升级标记 *************************/
  //标志位提示升级：SPI flash固件CRC校验 -> 擦除B区 -> 写B区 -> 芯片flash固件CRC校验 ->成功跳转B区，失败跳转A区
  if(para.flag == BOOT_FLAG_UPDATE_READY)
  {
      printf("Boot: detect UPDATE_READY flag, start firmware update\r\n");

      //----------① 先判长度合法性，再校验W25Q缓存中待升级固件CRC32 ----------
      // fw_len 必须非0、4字节对齐、不超过B区容量；否则CRC接口一律返回0，
      // 而0同时又是"输入非法"的返回值，旧代码用 (crc == 0) 判错存在歧义。
      if(boot_len_is_valid(para.fw_len, APP_B_SIZE) == 0)
      {
          printf("Boot: invalid fw_len=%lu! rollback to A(Golden)\r\n",
                 (unsigned long)para.fw_len);
          para.flag = BOOT_FLAG_IDLE;
          (void)boot_write_double_para(&para);
          boot_jump_or_halt(0);
      }

      uint32_t calc_w25q_crc = calc_w25q_fw_crc(W25Q_FW_BUF_ADDR, para.fw_len);

      // 长度已在上一步确认合法，这里只比较CRC值本身
      if(calc_w25q_crc != para.fw_crc32)
      {
          printf("Boot: W25Q firmware crc mismatch! rollback to A(Golden)\r\n");
          para.flag = BOOT_FLAG_IDLE;
          (void)boot_write_double_para(&para);
          boot_jump_or_halt(0);   // 直接回滚A区防砖镜像
      }

      //----------② 擦除B区镜像：S6 + S7 两个扇区全部擦除（用宏，避免硬编码地址） ----------
      int ret_erase1 = boot_flash_erase_safe(RUN_B_SECTOR1_ADDR);
      int ret_erase2 = boot_flash_erase_safe(RUN_B_SECTOR2_ADDR);
      if(ret_erase1 != 0 || ret_erase2 != 0)
      {
          printf("Boot: erase B area fail! e1=%d e2=%d, rollback to A\r\n",
                 ret_erase1, ret_erase2);
          para.flag = BOOT_FLAG_IDLE;
          (void)boot_write_double_para(&para);
          boot_jump_or_halt(0);
      }

      //----------③ 将W25Q固件拷贝写入片内B区 RUN_B_ADDR ----------
      int copy_ret = boot_copy_w25q_to_internal_flash(W25Q_FW_BUF_ADDR, RUN_B_ADDR, para.fw_len);
      if(copy_ret != 0)
      {
          printf("Boot: copy firmware to internal flash fail, ret=%d\r\n", copy_ret);
          para.flag = BOOT_FLAG_IDLE;
          (void)boot_write_double_para(&para);
          boot_jump_or_halt(0);
      }

      //----------④ 升级完成：计算B区实际固件CRC，更新参数，清除升级标记 ----------
      // 注意：使用实际固件长度para.fw_len，而非整个分区大小APP_B_SIZE
      uint32_t new_b_crc = boot_calc_flash_crc(RUN_B_ADDR, para.fw_len);
      if(new_b_crc == 0U)
      {
          printf("Boot: calc B crc fail\r\n");
          para.flag = BOOT_FLAG_IDLE;
          (void)boot_write_double_para(&para);
          boot_jump_or_halt(0);
      }

      para.appB_crc = new_b_crc;
      para.flag = BOOT_FLAG_IDLE;
      if(boot_write_double_para(&para) != 0)
      {
          // 写参数读回校验失败：只告警不中断，B区镜像已经写入成功
          printf("Boot: WARN para write verify fail\r\n");
      }
      printf("Boot: firmware update complete\r\n");
  }

  /*************************【正常启动分支：优先启动B-Run镜像】*************************/
  uint8_t b_image_ok = 0;

  // 前置条件：fw_len 必须合法。
  // 否则 boot_calc_flash_crc(RUN_B_ADDR, 0) 返回0，而默认参数的 appB_crc 也是0，
  // 0 == 0 会判定"校验通过"直接跳B——两份参数全损坏时反而最宽松，属于失败开放。
  if(boot_len_is_valid(para.fw_len, APP_B_SIZE) != 0)
  {
      // 第一步：MSP栈指针基础校验
      if(boot_check_msp_valid(RUN_B_ADDR))
      {
          // 第二步：完整固件CRC32校验
          uint32_t b_crc_now = boot_calc_flash_crc(RUN_B_ADDR, para.fw_len);
          if((b_crc_now != 0U) && (b_crc_now == para.appB_crc))
          {
              b_image_ok = 1;
          }
      }
  }

  if(b_image_ok)
  {
      printf("Boot: jump to B(Run App)\r\n");
      boot_jump_or_halt(1);
  }
  else
  {
      printf("Boot: B image invalid, try golden A\r\n");
      /* B区校验失败，回滚尝试A-Golden防砖镜像 */
      if(boot_check_msp_valid(GOLDEN_A_ADDR))
      {
          // 量产建议：此处增加A区完整CRC32校验，A区CRC可出厂预置宏定义
          printf("Boot: jump to A(Golden App)\r\n");
          boot_jump_or_halt(0);
      }
      else
      {
          /* A、B镜像全部损坏：进入串口救砖，等待上位机下发固件 */
          printf("Boot: A & B both invalid! enter uart recovery\r\n");
          while(boot_recover_uart() != 0)
          {
              printf("Boot: recovery failed, retry in 1s...\r\n");
              HAL_Delay(1000U);
          }
          /* boot_recover_uart() 成功后会自行软复位，不会返回0，这里只是保险 */
          while(1)
          {
          }
      }
  }
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 4;
  RCC_OscInitStruct.PLL.PLLN = 100;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 4;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_3) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  Period elapsed callback in non blocking mode
  * @note   This function is called  when TIM1 interrupt took place, inside
  * HAL_TIM_IRQHandler(). It makes a direct call to HAL_IncTick() to increment
  * a global variable "uwTick" used as application time base.
  * @param  htim : TIM handle
  * @retval None
  */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  /* USER CODE BEGIN Callback 0 */

  /* USER CODE END Callback 0 */
  if (htim->Instance == TIM1)
  {
    HAL_IncTick();
  }
  /* USER CODE BEGIN Callback 1 */

  /* USER CODE END Callback 1 */
}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
