add_library_unity(glue_grammar OBJECT glue_grammar.cpp)
set(GLUE_EXTENSION_FILES
    ${GLUE_EXTENSION_FILES} $<TARGET_OBJECTS:glue_grammar>
    PARENT_SCOPE)
