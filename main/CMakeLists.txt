file(GLOB_RECURSE MY_SRCS
    "apps/*.c"
    "apps/*.cc"
    "apps/*.cpp"
    "assets/*.c"
    "assets/*.cc"
    "assets/*.cpp"
    "hal/*.c"
    "hal/*.cc"
    "hal/*.cpp"
)

set(MY_EXTRA_SRCS
    "apps/app_guruguru/app_guruguru.cpp"
    "apps/app_guruguru/view/view.cpp"
    "apps/app_stack_remote/app_stack_remote.cpp"
    "apps/app_stack_remote/view/view.cpp"
    "assets/images/guruguru/guruguru_avatar_images.c"
    "assets/images/icon_stack_remote.c"
)
list(APPEND MY_SRCS ${MY_EXTRA_SRCS})
list(REMOVE_DUPLICATES MY_SRCS)

set(MY_INCS
    "."
)

idf_component_register(
    SRCS 
        "main.cpp"
        "wifi_time_sync.cpp"
        "phone_notifications.cpp"
        ${MY_SRCS}
    INCLUDE_DIRS 
        ${MY_INCS}
    EMBED_FILES
        "assets/sfx/boot_sfx.bin"
    EMBED_TXTFILES
        "hal/utils/config_ap/assets/badge_config_ap.html"
)
