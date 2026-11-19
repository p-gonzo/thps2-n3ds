# Writes thps2_build_id.h with the commits a 3DS build was built from, at
# BUILD time (not configure time), so log.txt names the binary's real source.
# "-dirty" marks a build with uncommitted changes. Rewritten only on change,
# so it causes a recompile only when the ids move.
# Inputs: SRC (title repo), PSX (psxrecomp checkout), OUT (header path).
foreach(_repo SRC PSX)
    execute_process(COMMAND git describe --always --dirty --abbrev=7
                    WORKING_DIRECTORY "${${_repo}}"
                    OUTPUT_VARIABLE _id_${_repo} OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(NOT _id_${_repo})
        set(_id_${_repo} "unknown")
    endif()
endforeach()
set(_text "#define THPS2_N3DS_BUILD \"${_id_SRC}\"\n#define THPS2_N3DS_PSXRECOMP \"${_id_PSX}\"\n")
if(EXISTS "${OUT}")
    file(READ "${OUT}" _old)
else()
    set(_old "")
endif()
if(NOT _old STREQUAL _text)
    file(WRITE "${OUT}" "${_text}")
endif()
