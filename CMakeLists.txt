cmake_minimum_required(VERSION 3.5)
set(CMAKE_CXX_STANDARD 17)


if (ESP_PLATFORM)

    idf_component_register(
        SRCS "./include/openocpp/implementation/logging_esp.cc"
        INCLUDE_DIRS "./include" "./rapidjson/include"
        PRIV_REQUIRES spiffs zlib
    )

    add_compile_definitions(JSON_NO_IO)

    return()

endif()

