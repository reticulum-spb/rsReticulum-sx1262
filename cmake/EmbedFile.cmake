if(NOT DEFINED INPUT OR NOT DEFINED OUTPUT)
    message(FATAL_ERROR "EmbedFile.cmake requires INPUT and OUTPUT")
endif()

file(READ "${INPUT}" CONTENT_HEX HEX)
string(REGEX REPLACE "(..)" "0x\\1, " CONTENT_BYTES "${CONTENT_HEX}")
file(WRITE "${OUTPUT}"
    "#ifndef RNS_SX1262_GENERATED_SCHEMA_H\n"
    "#define RNS_SX1262_GENERATED_SCHEMA_H\n\n"
    "static const unsigned char SX1262_CONFIG_SCHEMA[] = {\n"
    "    ${CONTENT_BYTES}0x00\n"
    "};\n\n"
    "#endif\n")
