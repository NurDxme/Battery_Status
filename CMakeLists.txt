cmake_minimum_required(VERSION 3.25)
project(Battery_Status C CXX RC)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# WIN32 = GUI-Programm ohne Konsolenfenster
add_executable(Battery_Status WIN32 main.cpp apple_ble.c resource.rc)
target_compile_definitions(Battery_Status PRIVATE UNICODE _UNICODE)
target_link_libraries(Battery_Status PRIVATE setupapi runtimeobject shell32 user32 gdi32 advapi32)

if (MINGW)
    # Laufzeitbibliotheken statisch einbinden, damit die .exe ohne MinGW im PATH startet
    target_link_options(Battery_Status PRIVATE -static-libgcc -static-libstdc++)
    # Release: Debug-Symbole entfernen (kleinere .exe)
    target_link_options(Battery_Status PRIVATE $<$<CONFIG:Release>:-s>)

    # libwinpthread lässt sich mit dieser MinGW-Version nicht statisch linken –
    # daher die DLL neben die .exe kopieren.
    get_filename_component(MINGW_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
    if (EXISTS "${MINGW_BIN}/libwinpthread-1.dll")
        add_custom_command(TARGET Battery_Status POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${MINGW_BIN}/libwinpthread-1.dll" "$<TARGET_FILE_DIR:Battery_Status>")
    endif()
endif()
