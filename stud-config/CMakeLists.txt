add_library(stud_config STATIC
    src/settings.cpp
)
target_include_directories(stud_config PUBLIC include)
target_link_libraries(stud_config PUBLIC nlohmann_json::nlohmann_json)
