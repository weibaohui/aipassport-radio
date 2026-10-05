# 网络收音机 应用组件(挂在 aipassport-fw 框架之上)。
#
# 通用能力(WiFi / 门户 / 存储 / UI 骨架 / 熄屏 / 按键 / BSP)全部来自
# components/appfw;本组件只保留业务:电台列表、HTTP 流客户端与 ICY 解析、
# MP3 解码播放、列表 UI 与门户卡片。
idf_component_register(
    SRC_DIRS "."
    INCLUDE_DIRS "."
    REQUIRES appfw bsp esp_timer nvs_flash esp_audio_codec json esp_http_client mbedtls
)

# 中文字库由框架提供(appfw/fonts/,常见 3500 字全量),无需应用自带;
# 自带子集字库的旧应用用同名强符号覆盖即可。
