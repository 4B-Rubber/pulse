# 悬停提示与提示延迟

图标条、工具栏这类只有图标的地方靠悬停提示说明自己。提示可以整体关掉，出现前的等待也可以调。

## 行为

- **覆盖范围**：窗口标签页、工具栏按钮、设置行、列表行（完整文件名或路径），以及侧栏折叠成图标条时的每一行。
- **开关**：设置 → 通用 → 显示悬停提示（`show_tooltips`，默认开）。
- **延迟**：短 / 标准 / 长 = 150 / 400 / 800 ms，另有“自定义”可填 50–5000 ms（`tooltip_delay_ms`）。
- **自定义编辑器**：设置行内就地弹出数字输入框，回车或点行外提交。点短 / 标准 / 长会先关掉这个编辑框再应用预设——否则行仍显示“自定义”，而且编辑框下一次提交会把旧数字写回去，看起来像预设没生效。
- **渲染**：文本和位置由窗口状态给出（`tooltipText` / `hoverPoint` / `hoverRegion`），画在窗口自绘层里，不创建系统 tooltip 窗口，因此跟随主题、也不会在截图与测试里缺失。

## 实现入口

- 偏好：`src/app/app_prefs.{h,cpp}`（`show_tooltips`、`tooltip_delay_ms`）。
- 设置行与行内编辑框：`src/app/settings_ui.cpp`（`SettingsTooltipDelay` 命中、`Show/HideTooltipDelayEditor`）、`src/app/app_hosted_edit.cpp`。
- 命中与绘制：`src/ui/ui_hit_test.*`、`src/ui/ui_renderer*.cpp`、`src/app/app_runtime.cpp`。
- 用例：`PULSE_SELFTEST_CASE=tooltip-delay-editor`（含“点预设要关掉自定义编辑器并立即生效”）。
