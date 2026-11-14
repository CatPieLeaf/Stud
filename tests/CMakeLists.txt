if(STUD_BUILD_UI)
    find_package(Qt6 REQUIRED COMPONENTS DBus)
    # These two use what the desktop provides, so each reports skipped on
    # a machine without it, a clean chroot or a CI container among them.
    add_executable(stud_gpu_enum_test gpu_enum_test.cpp)
    target_link_libraries(stud_gpu_enum_test PRIVATE stud_ui_gpu_enum)
    add_test(NAME gpu_enum_test COMMAND stud_gpu_enum_test)

    add_executable(stud_notifications_test notifications_test.cpp)
    target_link_libraries(stud_notifications_test PRIVATE stud_ui_notifications Qt6::DBus)
    add_test(NAME notifications_test COMMAND stud_notifications_test)

    set_tests_properties(gpu_enum_test notifications_test PROPERTIES SKIP_RETURN_CODE 77)

    # roblox-player: and roblox: launch-URI parsing.
    add_executable(stud_launch_uri_test launch_uri_test.cpp)
    target_link_libraries(stud_launch_uri_test PRIVATE stud_ui_launch_uri)
    add_test(NAME launch_uri_test COMMAND stud_launch_uri_test)

    # The log export writes a gzipped tar by hand; this checks the result
    # against the format, and against a real tar where there is one.
    add_executable(stud_log_archive_test log_archive_test.cpp)
    target_link_libraries(stud_log_archive_test PRIVATE stud_ui_log_archive)
    add_test(NAME log_archive_test COMMAND stud_log_archive_test)

    # Unpacking the client: symbolic links, and nothing outside the folder.
    add_executable(stud_zip_extract_test zip_extract_test.cpp)
    target_link_libraries(stud_zip_extract_test PRIVATE stud_ui_mac_client)
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/zip_scratch)
    add_test(NAME zip_extract_test COMMAND stud_zip_extract_test
             ${CMAKE_CURRENT_BINARY_DIR}/zip_scratch)
endif()

# Settings storage.
add_executable(stud_settings_test settings_test.cpp)
target_link_libraries(stud_settings_test PRIVATE stud_config)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/settings_scratch)
add_test(NAME settings_test COMMAND stud_settings_test
         ${CMAKE_CURRENT_BINARY_DIR}/settings_scratch)

# Every STUD_* variable the source reads is in docs/environment.md.
#
# The source root is passed in rather than discovered, so the test scans
# the tree it was built from wherever ctest runs it.
add_executable(stud_env_documented_test env_documented_test.cpp)
add_test(NAME env_documented_test
         COMMAND stud_env_documented_test "${CMAKE_SOURCE_DIR}")

# What of the client's output may reach the session log.
add_executable(stud_log_scrub_test log_scrub_test.cpp)
target_include_directories(stud_log_scrub_test PRIVATE ${CMAKE_SOURCE_DIR}/ui/src)
add_test(NAME log_scrub_test COMMAND stud_log_scrub_test)
