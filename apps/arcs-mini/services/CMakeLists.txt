listenai_library_named(app-services)
listenai_library_sources(
    service_led.c
    service_brightness.c
    service_volume.c
    service_button.c
    service_camera.c
    service_alarm.c
    service_image.c
    service_sd_music.c
    service_power_policy.c
)

listenai_include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
)

if(CONFIG_STOCK_WATCH)
    listenai_library_sources(../stock/service_stock.c ../stock/stock_quote.c
                            ../stock/stock_lookup.c ../stock/stock_http.c
                            ../stock/stock_chart.c ../stock/service_stock_chart.c)
    listenai_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../stock)
    listenai_library_sources_ifdef(CONFIG_LISA_SHELL ../stock/stock_shell.c)
endif()
