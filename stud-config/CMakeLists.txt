include(FetchContent)
if(NOT TARGET nlohmann_json::nlohmann_json)
    FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG        v3.11.3
    )
    set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(nlohmann_json)
endif()

add_library(stud_config STATIC
    src/settings.cpp
)
target_include_directories(stud_config PUBLIC include)
target_link_libraries(stud_config PUBLIC nlohmann_json::nlohmann_json)
