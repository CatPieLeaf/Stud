add_library(stud_bionic_runtime STATIC
    src/bionic_runtime.cpp
    src/property_area.cpp
)
target_include_directories(stud_bionic_runtime PUBLIC include)

target_compile_definitions(stud_bionic_runtime PRIVATE
    STUD_THIRD_PARTY_DIR="${STUD_THIRD_PARTY_DIR}")
