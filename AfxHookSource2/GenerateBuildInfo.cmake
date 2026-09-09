if(NOT DEFINED SOURCE_DIR OR NOT DEFINED OUTPUT_FILE)
    message(FATAL_ERROR "SOURCE_DIR and OUTPUT_FILE are required")
endif()

set(build_revision "unknown")
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --short=8 HEAD
        WORKING_DIRECTORY "${SOURCE_DIR}"
        OUTPUT_VARIABLE revision
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE revision_result
    )
    if(revision_result EQUAL 0 AND revision MATCHES "^[0-9a-fA-F]+$")
        set(build_revision "${revision}")
        # Ignore untracked outputs, but include staged/unstaged tracked edits.
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" diff --quiet --ignore-submodules=untracked HEAD --
            WORKING_DIRECTORY "${SOURCE_DIR}"
            ERROR_QUIET
            RESULT_VARIABLE dirty_result
        )
        if(dirty_result EQUAL 1)
            string(APPEND build_revision "-dirty")
        elseif(NOT dirty_result EQUAL 0)
            string(APPEND build_revision "-state-unknown")
        endif()
    endif()
endif()
if(build_revision STREQUAL "unknown")
    message(WARNING "Git revision unavailable; build log will report unknown")
endif()

string(TIMESTAMP build_time "%Y-%m-%dT%H:%M:%SZ" UTC)
get_filename_component(output_dir "${OUTPUT_FILE}" DIRECTORY)
file(MAKE_DIRECTORY "${output_dir}")
file(WRITE "${OUTPUT_FILE}"
    "// Generated at build time. Do not edit.\n#pragma once\n"
    "#define AFX_BUILD_GIT \"${build_revision}\"\n"
    "#define AFX_BUILD_UTC \"${build_time}\"\n"
)
