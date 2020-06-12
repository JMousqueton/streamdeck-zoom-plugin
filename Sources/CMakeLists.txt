set(CMAKE_CXX_STANDARD 20)

set(
  SOURCES
  MyStreamDeckPlugin.cpp
  main.cpp
)

add_executable(
  sdzoomplugin
  ${SOURCES}
  ${PLATFORM_FILES}
)
target_link_libraries(sdzoomplugin json StreamDeckSDK)
install(TARGETS sdzoomplugin DESTINATION ${CMAKE_INSTALL_PREFIX})
