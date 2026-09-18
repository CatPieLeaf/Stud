add_library(stud_ipc STATIC
    src/ipc.cpp
)
target_include_directories(stud_ipc PUBLIC include)
target_link_libraries(stud_ipc PUBLIC nlohmann_json::nlohmann_json)
