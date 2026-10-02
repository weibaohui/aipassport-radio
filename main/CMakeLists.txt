# 网络收音机 应用组件(挂在 aipassport-fw 框架之上)。
#
# 通用能力(WiFi / 门户 / 存储 / UI 骨架 / 熄屏 / 按键 / BSP)全部来自
# components/appfw;本组件只保留业务:电台列表、HTTP 流客户端与 ICY 解析、
# MP3 解码播放、列表 UI 与门户卡片。
idf_component_register(
    SRC_DIRS "."
    INCLUDE_DIRS "."
    REQUIRES appfw bsp esp_timer nvs_flash esp_audio_codec json
)

# 中文字体子集(生成物,来源/命令/字符清单见 assets/fonts/README.md)。
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/app_font_16.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/app_font_24.c"
)
