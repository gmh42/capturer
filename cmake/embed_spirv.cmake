# Converts a SPIR-V binary into a C++ source defining `const unsigned char <SYMBOL>[]` and `<SYMBOL>_size`.
# Usage: cmake -DINPUT=file.spv -DOUTPUT=file.cpp -DSYMBOL=name -P embed_spirv.cmake
file(READ "${INPUT}" content HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${content}")
file(WRITE "${OUTPUT}"
"namespace capturer::gpu::shaders {
extern const unsigned char ${SYMBOL}[];
extern const unsigned long ${SYMBOL}_size;
const unsigned char ${SYMBOL}[] = {${bytes}};
const unsigned long ${SYMBOL}_size = sizeof(${SYMBOL});
}
")
