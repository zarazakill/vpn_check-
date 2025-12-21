cmake_minimum_required(VERSION 3.16)
project(VPNGateManager)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Находим Qt6 компоненты
find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network Concurrent)

# Если нужны дополнительные модули для будущих расширений
# find_package(Qt6 COMPONENTS Quick Qml)

set(SOURCES
    main.cpp
    mainwindow.cpp
    serverdownloader.cpp
    vpnmanager.cpp
    servertester.cpp  # Добавляем сервер тестер
)

set(HEADERS
    mainwindow.h
    vpntypes.h
    serverdownloader.h
    vpnmanager.h
    servertester.h    # Добавляем сервер тестер
)

set(FORMS
    mainwindow.ui
)

add_executable(VPNGateManager ${SOURCES} ${HEADERS} ${FORMS})

# Подключаем Qt6 модули
target_link_libraries(VPNGateManager
    Qt6::Core
    Qt6::Widgets
    Qt6::Network
    Qt6::Concurrent
)

# Для Linux добавляем системные библиотеки
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(VPNGateManager pthread)
endif()

# Настройки компиляции
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(VPNGateManager PRIVATE QT_DEBUG)
    target_compile_options(VPNGateManager PRIVATE -g -O0 -Wall -Wextra)
else()
    target_compile_definitions(VPNGateManager PRIVATE QT_NO_DEBUG)
    target_compile_options(VPNGateManager PRIVATE -O2)
endif()

# Установка
install(TARGETS VPNGateManager DESTINATION bin)

# Копирование иконки приложения (если есть)
# if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/icons/app-icon.png")
#     install(FILES icons/app-icon.png DESTINATION share/icons)
# endif()

# Создание .desktop файла для Linux (опционально, можно добавить позже)
# if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/VPNGateManager.desktop.in")
#     configure_file(
#         "${CMAKE_CURRENT_SOURCE_DIR}/VPNGateManager.desktop.in"
#         "${CMAKE_CURRENT_BINARY_DIR}/VPNGateManager.desktop"
#         @ONLY
#     )
#     install(FILES "${CMAKE_CURRENT_BINARY_DIR}/VPNGateManager.desktop"
#             DESTINATION share/applications)
# endif()
