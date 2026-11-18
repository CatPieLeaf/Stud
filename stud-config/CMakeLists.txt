find_package(Qt6 REQUIRED COMPONENTS Core)

add_library(stud_config STATIC
    src/settings.cpp
)
target_include_directories(stud_config PUBLIC include)
target_link_libraries(stud_config PUBLIC Qt6::Core)
