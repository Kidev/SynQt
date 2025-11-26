# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# synqt_add_contract. Lower the .syn contracts under generated/ into the QtRO layer and
# wire the result into a target. Runs the synqtc generator (.syn -> .rep + Source helper +
# Replica registration), then drives repc via qt_add_repc_sources (owners) or
# qt_add_repc_replicas (consumers). A service-only target takes ROLE source. The
# client takes ROLE replica, so a Source helper never links into the client.
#
#   synqt_add_contract(<target>
#       ROLE source|replica|both        # which side(s) to generate for this target
#       [FORWARDS_SESSION]              # a service consumes it, so calls carry the session
#                                       # the calling entity is acting for
#       SYN <file.syn> [<file.syn> ...]  # the contracts
#   )
#
# FORWARDS_SESSION has to match on both sides of a link. It changes the slot signatures in
# the rep, so an owner compiled with it and a consumer compiled without would disagree about
# the API. The build derives it from the topology, so the two always agree.
#
# The generator runs at configure time. Editing a .syn or the generator itself
# re-runs CMake (and thus regenerates) automatically.

find_package(Python3 REQUIRED COMPONENTS Interpreter)

# The directory that contains the synqtc package (this file lives in <repo>/cmake).
get_filename_component(_SYNQT_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(SYNQTC_ROOT "${_SYNQT_REPO_ROOT}/tools/synqtc" CACHE INTERNAL "synqtc package root")
# Cached like the above, because synqt_add_contract resolves src/contract through it and a
# function reads the scope it is called from, not the one this file was included in.
set(SYNQT_REPO_ROOT "${_SYNQT_REPO_ROOT}" CACHE INTERNAL "SynQt repository root")

# Editing the generator reruns codegen, as editing a .syn does.
file(GLOB _SYNQTC_SOURCES CONFIGURE_DEPENDS "${SYNQTC_ROOT}/synqtc/*.py")
set(SYNQTC_SOURCES "${_SYNQTC_SOURCES}" CACHE INTERNAL "synqtc generator sources")

# Write only on a content change: file(WRITE) always moves the timestamp and forces a
# recompile of every includer.
function(_synqt_write_if_changed path content)
    if(EXISTS "${path}")
        file(READ "${path}" _existing)
        if(_existing STREQUAL "${content}")
            return()
        endif()
    endif()
    file(WRITE "${path}" "${content}")
endfunction()

function(synqt_add_contract target)
    cmake_parse_arguments(ARG "FORWARDS_SESSION" "ROLE" "SYN" ${ARGN})
    if(NOT ARG_ROLE)
        set(ARG_ROLE "both")
    endif()
    if(NOT ARG_ROLE MATCHES "^(source|replica|both)$")
        message(FATAL_ERROR "synqt_add_contract: ROLE must be source, replica, or both")
    endif()
    if(NOT ARG_SYN)
        message(FATAL_ERROR "synqt_add_contract: no SYN files given")
    endif()

    # Mandatory: SourceModel is what keeps consumers from writing into a published model.
    if(NOT ARG_ROLE STREQUAL "replica")
        if(NOT TARGET SynQtContract)
            add_subdirectory("${SYNQT_REPO_ROOT}/src/contract"
                "${CMAKE_BINARY_DIR}/SynQtContract")
        endif()
        target_link_libraries(${target} PRIVATE SynQtContract)
    endif()

    set(gen_flags "")
    if(ARG_FORWARDS_SESSION)
        list(APPEND gen_flags "--forwards-session")
    endif()

    # Per-target so several targets in one directory (owner, consumer, and a both-
    # sided test) can compile the same contracts with different roles without their
    # role-specific rep indirection headers colliding.
    set(gendir "${CMAKE_CURRENT_BINARY_DIR}/synqt_generated/${target}")
    file(MAKE_DIRECTORY "${gendir}")

    # repc builds an include guard from the .rep's basename unsanitised, so this must be
    # an identifier.
    string(MAKE_C_IDENTIFIER "${target}" target_id)

    foreach(syn IN LISTS ARG_SYN)
        get_filename_component(syn_abs "${syn}" ABSOLUTE)
        get_filename_component(stem "${syn}" NAME_WE)
        string(TOLOWER "${stem}" lstem)

        # Regenerate now, and re-run CMake if the contract or the generator changes.
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            "${syn_abs}" ${SYNQTC_SOURCES})
        execute_process(
            COMMAND "${Python3_EXECUTABLE}" -m synqtc "${syn_abs}" --out "${gendir}" --quiet
                    ${gen_flags}
            WORKING_DIRECTORY "${SYNQTC_ROOT}"
            RESULT_VARIABLE _gen_rc
            OUTPUT_VARIABLE _gen_out
            ERROR_VARIABLE _gen_err
        )
        if(NOT _gen_rc EQUAL 0)
            message(FATAL_ERROR "synqtc failed for ${syn}:\n${_gen_err}")
        endif()

        # repc writes into the directory's binary dir, named after the .rep, so the copy it
        # gets carries the target's name: two targets in one directory with the same contract
        # and role would otherwise generate the same files.
        set(rep "${gendir}/${lstem}__${target_id}.rep")
        configure_file("${gendir}/${lstem}.rep" "${rep}" COPYONLY)
        # repc calls the both-sided output "merged", not "both".
        set(repc_kind "${ARG_ROLE}")
        if(repc_kind STREQUAL "both")
            set(repc_kind "merged")
        endif()
        set(repc_header "rep_${lstem}__${target_id}_${repc_kind}.h")

        # ROLE both must use the merged header, where a POD is emitted once. The helper
        # includes "<lstem>_rep.h", which points at the header for this target's role.
        if(ARG_ROLE STREQUAL "source")
            qt_add_repc_sources(${target} "${rep}")
            target_sources(${target} PRIVATE "${gendir}/${lstem}_sourcehelper.cpp")
        elseif(ARG_ROLE STREQUAL "replica")
            qt_add_repc_replicas(${target} "${rep}")
            target_sources(${target} PRIVATE
                "${gendir}/${lstem}_replica.cpp"
                "${gendir}/${lstem}_consumer.cpp")
        else() # both
            qt_add_repc_merged(${target} "${rep}")
            target_sources(${target} PRIVATE
                "${gendir}/${lstem}_sourcehelper.cpp"
                "${gendir}/${lstem}_replica.cpp"
                "${gendir}/${lstem}_consumer.cpp")
        endif()
        _synqt_write_if_changed("${gendir}/${lstem}_rep.h"
            "#include \"${repc_header}\"\n")
        # The name repc would have produced without the per-target suffix, for code that
        # includes it directly (a test constructing a POD, a library exposing one).
        _synqt_write_if_changed("${gendir}/rep_${lstem}_${repc_kind}.h"
            "#include \"${repc_header}\"\n")
    endforeach()

    # The Source helper #includes the repc-generated headers, which repc emits into the
    # directory's binary dir. The generated files themselves live under synqt_generated/.
    target_include_directories(${target} PRIVATE "${gendir}" "${CMAKE_CURRENT_BINARY_DIR}")
endfunction()
