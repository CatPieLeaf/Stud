add_library(stud_render STATIC src/render.cpp)
target_include_directories(stud_render PUBLIC include)
target_link_libraries(stud_render PUBLIC ${CMAKE_DL_LIBS})
