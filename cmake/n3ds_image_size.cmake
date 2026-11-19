# Fails the 3DS build when the program image (code + data + bss) gets close
# to 64 MB: Luma3DS's loader maps the image at 0x10000000 and itself at
# 0x14000000, so a larger image crashes on real hardware before main (Azahar
# does not enforce it). Inputs: SIZE (arm-none-eabi-size), ELF, LIMIT_MB.
execute_process(COMMAND "${SIZE}" "${ELF}" OUTPUT_VARIABLE _out RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "image size check: ${SIZE} failed")
endif()
string(REGEX MATCH "\n[ \t]*([0-9]+)[ \t]+([0-9]+)[ \t]+([0-9]+)" _m "${_out}")
math(EXPR _total "${CMAKE_MATCH_1} + ${CMAKE_MATCH_2} + ${CMAKE_MATCH_3}")
math(EXPR _limit "${LIMIT_MB} * 1048576")
math(EXPR _mb10 "${_total} * 10 / 1048576")
math(EXPR _mb "${_mb10} / 10")
math(EXPR _frac "${_mb10} % 10")
if(_total GREATER _limit)
    message(FATAL_ERROR "3DS program image is ${_mb}.${_frac} MB, over the ${LIMIT_MB} MB budget "
                        "(hard limit 64 MB: Luma's loader). Move large static buffers to the heap.")
endif()
message(STATUS "3DS program image ${_mb}.${_frac} MB (budget ${LIMIT_MB} MB)")
