add_subdirectory(api)
add_subdirectory(catalog)
add_subdirectory(core)
add_subdirectory(functions)
add_subdirectory(grammar)
add_subdirectory(planning)

add_library_unity(glue_main OBJECT glue_extension.cpp)
target_link_libraries(glue_main ${AWSSDK_LINK_LIBRARIES})

set(GLUE_EXTENSION_FILES
    ${GLUE_EXTENSION_FILES} $<TARGET_OBJECTS:glue_main>
    PARENT_SCOPE)
