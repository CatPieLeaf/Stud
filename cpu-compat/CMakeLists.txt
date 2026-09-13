add_library(stud_cpu_compat STATIC
    src/cpu_compat.cpp
    src/x86_emulator.cpp
)
target_include_directories(stud_cpu_compat PUBLIC include)
