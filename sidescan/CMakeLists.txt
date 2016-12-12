
add_executable (side-scan side-scan.c)
target_link_libraries (side-scan ${GLIB2_LIBRARIES} ${GTK3_LIBRARIES} ${HYSCAN_LIBRARIES})
