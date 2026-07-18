# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# synqt_add_contract: lower shared/*.syn contracts into the QtRO layer and wire the
# result into a target. Runs the synqtc generator (.syn -> .rep + Source helper +
# Replica registration), then drives repc via qt_add_repc_sources (owners) or
# qt_add_repc_replicas (consumers). A service-only target takes ROLE source; the
# client takes ROLE replica, so a Source helper never links into the client.
#
#   synqt_add_contract(<target>
#       ROLE source|replica|both        # which side(s) to generate for this target
#       SYN <file.syn> [<file.syn> ...]  # the contracts
#   )
#
# The generator runs at configure time; editing a .syn or the generator itself
# re-runs CMake (and thus regenerates) automatically.

find_package(Python3 REQUIRED COMPONENTS Interpreter)

# The directory that contains the synqtc package (this file lives in <repo>/cmake).
get_filename_component(_SYNQT_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(SYNQTC_ROOT "${_SYNQT_REPO_ROOT}/tools/synqtc" CACHE INTERNAL "synqtc package root")

# The generator's own sources. Editing the generator (e.g. emit.py) must re-run
# codegen as surely as editing a .syn does, or a build silently keeps stale output.
# Glob once at include time and make every consumer directory depend on them below.
file(GLOB _SYNQTC_SOURCES CONFIGURE_DEPENDS "${SYNQTC_ROOT}/synqtc/*.py")
set(SYNQTC_SOURCES "${_SYNQTC_SOURCES}" CACHE INTERNAL "synqtc generator sources")

function(synqt_add_contract target)
    cmake_parse_arguments(ARG "" "ROLE" "SYN" ${ARGN})
    if(NOT ARG_ROLE)
        set(ARG_ROLE "both")
    endif()
    if(NOT ARG_ROLE MATCHES "^(source|replica|both)$")
        message(FATAL_ERROR "synqt_add_contract: ROLE must be source, replica, or both")
    endif()
    if(NOT ARG_SYN)
        message(FATAL_ERROR "synqt_add_contract: no SYN files given")
    endif()

    # Per-target so several targets in one directory (owner, consumer, and a both-
    # sided test) can compile the same contracts with different roles without their
    # role-specific rep indirection headers colliding.
    set(gendir "${CMAKE_CURRENT_BINARY_DIR}/synqt_generated/${target}")
    file(MAKE_DIRECTORY "${gendir}")

    foreach(syn IN LISTS ARG_SYN)
        get_filename_component(syn_abs "${syn}" ABSOLUTE)
        get_filename_component(stem "${syn}" NAME_WE)
        string(TOLOWER "${stem}" lstem)

        # Regenerate now, and re-run CMake if the contract or the generator changes.
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            "${syn_abs}" ${SYNQTC_SOURCES})
        execute_process(
            COMMAND "${Python3_EXECUTABLE}" -m synqtc "${syn_abs}" --out "${gendir}" --quiet
            WORKING_DIRECTORY "${SYNQTC_ROOT}"
            RESULT_VARIABLE _gen_rc
            OUTPUT_VARIABLE _gen_out
            ERROR_VARIABLE _gen_err
        )
        if(NOT _gen_rc EQUAL 0)
            message(FATAL_ERROR "synqtc failed for ${syn}:\n${_gen_err}")
        endif()

        set(rep "${gendir}/${lstem}.rep")

        # A rep with a POD defines its Q_GADGET in both the _source and _replica
        # headers, so a single target that is BOTH an owner and a consumer must use
        # the merged header (the POD is emitted once). Real entities are owner-only
        # (source) or consumer-only (replica) and never hit this. The helper and
        # replica sources include a stable "<lstem>_rep.h" indirection that we point
        # at the repc header matching this target's role.
        if(ARG_ROLE STREQUAL "source")
            qt_add_repc_sources(${target} "${rep}")
            file(WRITE "${gendir}/${lstem}_rep.h" "#include \"rep_${lstem}_source.h\"\n")
            target_sources(${target} PRIVATE "${gendir}/${lstem}_sourcehelper.cpp")
        elseif(ARG_ROLE STREQUAL "replica")
            qt_add_repc_replicas(${target} "${rep}")
            file(WRITE "${gendir}/${lstem}_rep.h" "#include \"rep_${lstem}_replica.h\"\n")
            target_sources(${target} PRIVATE
                "${gendir}/${lstem}_replica.cpp"
                "${gendir}/${lstem}_consumer.cpp")
        else() # both
            qt_add_repc_merged(${target} "${rep}")
            file(WRITE "${gendir}/${lstem}_rep.h" "#include \"rep_${lstem}_merged.h\"\n")
            target_sources(${target} PRIVATE
                "${gendir}/${lstem}_sourcehelper.cpp"
                "${gendir}/${lstem}_replica.cpp"
                "${gendir}/${lstem}_consumer.cpp")
        endif()
    endforeach()

    # The Source helper #includes the repc-generated headers (rep_<name>_source.h),
    # which repc emits into the target's binary dir; the generated files themselves
    # live under synqt_generated/.
    target_include_directories(${target} PRIVATE "${gendir}" "${CMAKE_CURRENT_BINARY_DIR}")
endfunction()
