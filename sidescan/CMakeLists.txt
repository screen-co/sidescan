
find_program (RESOURCE_COMPILER glib-compile-resources)
if (${RESOURCE_COMPILER} STREQUAL RESOURCE_COMPILER-NOTFOUND)
  message (FATAL_ERROR "GLib resource compiler not found.")
endif ()

file (MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/resources)
add_custom_command (OUTPUT "${CMAKE_BINARY_DIR}/resources/ame-side-scan-resources.c"
                    COMMAND ${RESOURCE_COMPILER} --generate-source
                            --target "${CMAKE_BINARY_DIR}/resources/ame-side-scan-resources.c"
                            side-scan-resources.xml
                    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                    DEPENDS side-scan.ui
                    VERBATIM)

if (NOT SONAR_DRIVERS_PATH)
  set (SONAR_DRIVERS_PATH ".")
endif ()
add_definitions (-DSONAR_DRIVERS_PATH="${SONAR_DRIVERS_PATH}")

add_executable (side-scan
                side-scan.c
                sonar-configure.c
                ${CMAKE_BINARY_DIR}/resources/ame-side-scan-resources.c)

target_link_libraries (side-scan ${GTK3_LIBRARIES} ${HYSCAN_LIBRARIES})

install (TARGETS side-scan
         COMPONENT runtime
         RUNTIME DESTINATION bin
         LIBRARY DESTINATION lib
         ARCHIVE DESTINATION lib
         PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
