add_library(stud_bionic_runtime STATIC
    src/bionic_runtime.cpp
    src/property_area.cpp
)
target_include_directories(stud_bionic_runtime PUBLIC include)
# stud/stud_paths.h, header-only: where the DNS property area lives.
target_include_directories(stud_bionic_runtime PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../android-glue/include)

target_compile_definitions(stud_bionic_runtime PRIVATE
    STUD_THIRD_PARTY_DIR="${STUD_THIRD_PARTY_DIR}")
