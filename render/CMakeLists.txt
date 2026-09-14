set(STUD_RENDER_SOURCES src/render.cpp)

if(STUD_ENABLE_DEV_RENDER_TOGGLE)
    include(FetchContent)
    # Same dependency jni-bridge already vendors for its own JSON config
    # (flag_overrides.cpp), guarded so re-declaring it here is a no-op
    # if jni-bridge's subdirectory already made it available first.
    if(NOT TARGET nlohmann_json::nlohmann_json)
        FetchContent_Declare(
            nlohmann_json
            GIT_REPOSITORY https://github.com/nlohmann/json.git
            GIT_TAG        v3.11.3
        )
        set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(nlohmann_json)
    endif()
    list(APPEND STUD_RENDER_SOURCES src/dev_backend_config.cpp)
endif()

add_library(stud_render STATIC ${STUD_RENDER_SOURCES})
target_include_directories(stud_render PUBLIC include)
target_link_libraries(stud_render PUBLIC ${CMAKE_DL_LIBS})

if(STUD_ENABLE_DEV_RENDER_TOGGLE)
    target_compile_definitions(stud_render PUBLIC STUD_ENABLE_DEV_RENDER_TOGGLE)
    target_link_libraries(stud_render PUBLIC nlohmann_json::nlohmann_json)
endif()

target_compile_definitions(stud_render PRIVATE
    STUD_THIRD_PARTY_DIR="${STUD_THIRD_PARTY_DIR}")
