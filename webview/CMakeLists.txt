# The web-view panel the Roblox app asks for over the MessageBus.
#
# QtWebEngine lives here and only here: Stud's login is still done inside
# the real Roblox app (no web login window, unchanged), and this viewer
# only ever renders pages the app itself requested, using the cookies the
# app's own engine handed over.
find_package(Qt6 REQUIRED COMPONENTS Widgets WebEngineWidgets Network)

add_executable(stud-webview src/main.cpp)
# stud/webview_user_agent.h, the agent string has to be the same one the
# engine reports to Roblox, so both build it from the same header.
target_include_directories(stud-webview PRIVATE
                            "${CMAKE_CURRENT_SOURCE_DIR}/../android-glue/include")
target_link_libraries(stud-webview PRIVATE Qt6::Widgets Qt6::WebEngineWidgets Qt6::Network)
set_target_properties(stud-webview PROPERTIES AUTOMOC ON)

install(TARGETS stud-webview RUNTIME DESTINATION "${STUD_LIBEXEC_DIR}")
