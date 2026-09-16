#ifndef __RESTART_H
#define __RESTART_H

#include "main.h"
#include "boot_param.h"
#define APP_A_OFFSET GOLDEN_A_OFFSET
#define APP_B_OFFSET RUN_B_OFFSET

/**
 * @brief  跳转到 APP 镜像
 * @param  i 0 = A-Golden 防砖镜像；1 = B-Run 可升级镜像
 * @retval 0 不应返回（已跳走）；-1 镜像非法，未跳转
 * @note   调用方必须处理返回值：成功跳转时函数不会返回，
 *         返回 -1 时应当换分区重试或进入死循环，不要让程序继续往下跑。
 */
int jump_to_app(uint8_t i);
#endif
