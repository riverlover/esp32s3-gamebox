/* 游戏装载进度页。
 *
 * 这是单任务、单实例界面：菜单返回后开始显示，模拟器接管屏幕后自然结束。
 * 进度回调会自行把刷新限制在每 5%，避免高延迟 TF 卡之外又叠加过多推屏时间。 */
#pragma once

void loading_screen_begin(const char *game_name);
void loading_screen_progress(const char *stage, unsigned percent);
void loading_screen_error(const char *message);
