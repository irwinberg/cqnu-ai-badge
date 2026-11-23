# 重师大 AI 工牌应用。字体/图片由 tools/cqnu_gen_*.py 生成到 assets/。
file(GLOB CQ_ASSET_SRCS "assets/fonts/*.c" "assets/images/*.c")
idf_component_register(
    SRCS "main.c"
         "app_nav.c"
         "app_model.c"
         "app_store.c"
         "app_sound.c"
         "app_net.c"
         "app_blufi_security.c"
         "app_ui.c"
         "my_feature.c"
         ${CQ_ASSET_SRCS}
    INCLUDE_DIRS "." "assets/images"
    REQUIRES bsp bt esp_event esp_hw_support esp_http_client esp_netif esp_timer esp_wifi json lwip mbedtls nvs_flash
)
