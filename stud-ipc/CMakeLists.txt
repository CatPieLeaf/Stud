add_library(stud_ipc STATIC
    src/ipc.cpp
)
target_include_directories(stud_ipc PUBLIC include)
# stud/stud_paths.h, header-only: where the launch socket lives.
target_include_directories(stud_ipc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../android-glue/include)
target_link_libraries(stud_ipc PUBLIC nlohmann_json::nlohmann_json)
