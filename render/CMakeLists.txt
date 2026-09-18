set(STUD_RENDER_SOURCES src/render.cpp)

if(STUD_ENABLE_DEV_RENDER_TOGGLE)
    # nlohmann_json is fetched once at the top level; see the root
    # CMakeLists.txt. The comment that used to sit here named jni-bridge,
    # a directory that no longer exists.
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
