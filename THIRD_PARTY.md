# 第三方资料与字体

- `firmware/main/board_display.c` 的屏幕与触摸适配改编自[微雪 3.49 V2 官方示例](https://github.com/waveshareteam/ESP32-S3-Touch-LCD-3.49-V2)的 `10_LVGL_V9_Test`，供电保持依据 `07_BATT_PWR_Test`。该适配保留来源说明，许可证全文见 `licenses/Waveshare-Apache-2.0.txt`。
- 机身按键、电池和 WiFi 代码复用了作者此前的 `pc-monitor` 工程，并随本项目提供源码，不需要外部同名目录。
- 中文、西文与数字使用 **MiSans Regular（400，正常字重）**，字体版权归 Xiaomi Inc.。字库仅作为本应用的一部分提供，适用独立的 [MiSans 字体许可](licenses/MiSans-License.pdf)，不适用项目的 Apache-2.0 许可。原始 TTF 不随仓库分发；下载和使用须遵守[小米官方字体与许可说明](https://hyperos.mi.com/font/zh/download/)。固件启动日志标明使用 MiSans 字体。
- `lv_font_conv` 生成 4 bpp 字形：正文 14 px，电量 12 px，主指标 32 px。正文包含 ASCII、Latin-1、CJK 基本区与扩展 A、中文标点及全角字符，使用 LVGL 字体压缩；12／32 px 只包含所需西文。Montserrat 仅为左右箭头、复选框等 LVGL 图标提供回退，不用于正文。
- ESP-IDF 组件版本固定在 `firmware/main/idf_component.yml`，组件许可证随组件管理器下载提供。
