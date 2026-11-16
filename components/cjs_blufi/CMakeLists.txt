
set(srcs doit_blufi_storage.cc)
if(CONFIG_USE_BLUFI_NET_CONFIGURING)
    list(APPEND srcs
            doit_blufi.cc
            blufi_init.c
            blufi_security.c
            )
endif()

list(APPEND incs
        .
        )

        

idf_component_register(SRCS "${srcs}"
                            INCLUDE_DIRS "${incs}"
                            EMBED_TXTFILES "${assets}"
                            REQUIRES driver esp_wifi bt esp_coex nvs_flash json esp_timer mbedtls esp_common 78__esp-wifi-connect)



