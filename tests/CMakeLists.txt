# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# Every test suite that runs on the host kit, in one build tree and under one ctest.
#
# Each suite is also a standalone project (its own project() call, its own run-*.sh) and
# stays one: a milestone has to be independently testable, and one suite on its own is how
# you bisect a failure. What this file adds is the whole-tree build, where the runtime
# libraries are compiled once instead of once per suite. Measured on a 32-core host, from
# clean: 17 suites configured and built one at a time cost 227 s and 812 object files; this
# tree costs 18 s and 295, because SynQtService and SynQtClient are built once and because
# 17 configure steps become one.
#
# In milestone order, which is the order the framework was built in and the order a failure
# reads best in: an M5 break with M2 green says something different from both breaking.
set(SYNQT_HOST_SUITES
    m1-contract
    m2-transport
    m3-mesh
    m4-topology
    m5-webedge
    m6-client
    m6-clientupdate
    consumer-facade
    m7-caller
    m8-auth
    m9-providers
    prov4-runtime
    fix1-auction
    fix2-arena
    fix3-stall
    url-routing
    remote-pages
    entity-test
    graphics
    memory
)

# Suites that cannot join the tree because CMake is not their entry point: each one runs a
# generator first and compiles what it produced, so there is nothing to add_subdirectory
# until its run-*.sh has run. They are what catch a tool whose output stopped compiling,
# which no test of the generated strings can, so they are listed rather than forgotten:
# published below for CI to run, and covered by the drift guard like everything else.
set(SYNQT_SCRIPT_SUITES
    custom-provider  # compiles the skeletons from `synqt add provider`
    appgen-native    # compiles the entity mains appgen.py emits for a whole topology
    desktop-client   # builds the client QML as a native desktop app
)

# Suites that belong to another toolchain, and therefore to another workflow.
set(SYNQT_SUITES_ELSEWHERE
    m0-transport         # WASM client and a native edge, in a browser; browser-matrix.yml
    wasm-quick3dphysics  # WASM only; wasm-proofs.yml
    split-origin         # two sites in a real browser, no Qt at all; browser-matrix.yml
    designer             # the design editor in a browser, no Qt at all; tests.yml
)

# A hand-maintained list cannot report what is missing from it, which is how remote-pages
# reached 1849 lines and five commits without CI ever running it. So ask the directory
# instead: anything with a CMakeLists.txt or a runner has to appear in exactly one of the
# three lists above, and configuring the tree is what enforces it, locally as well as in
# CI. Deciding a suite is out is fine; leaving it undecided is not.
file(GLOB _synqt_suite_files
    RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/*/CMakeLists.txt"
    "${CMAKE_CURRENT_SOURCE_DIR}/*/run-*.sh")
set(_synqt_unlisted "")
foreach(_synqt_suite_file IN LISTS _synqt_suite_files)
    get_filename_component(_synqt_suite "${_synqt_suite_file}" DIRECTORY)
    if(NOT _synqt_suite IN_LIST SYNQT_HOST_SUITES
            AND NOT _synqt_suite IN_LIST SYNQT_SCRIPT_SUITES
            AND NOT _synqt_suite IN_LIST SYNQT_SUITES_ELSEWHERE)
        list(APPEND _synqt_unlisted "${_synqt_suite}")
    endif()
endforeach()
if(_synqt_unlisted)
    list(REMOVE_DUPLICATES _synqt_unlisted)
    list(JOIN _synqt_unlisted " " _synqt_unlisted_text)
    message(FATAL_ERROR
        "tests/ holds suites this tree neither builds nor accounts for: "
        "${_synqt_unlisted_text}\n"
        "Add each to SYNQT_HOST_SUITES, or to SYNQT_SCRIPT_SUITES or "
        "SYNQT_SUITES_ELSEWHERE with the reason it is out.")
endif()

# The same argument, one level out. docs/development.md catalogues these suites for whoever
# is about to work on one, and that table is a second hand-maintained list, so it drifts the
# same way: split-origin ran in CI for weeks while the page it should have been on had never
# heard of it. A registered suite has to be findable by a reader, not only by ctest.
set(_synqt_dev_docs "${CMAKE_CURRENT_SOURCE_DIR}/../docs/development.md")
if(EXISTS "${_synqt_dev_docs}")
    file(READ "${_synqt_dev_docs}" _synqt_dev_docs_text)
    set(_synqt_undocumented "")
    foreach(_synqt_suite IN LISTS SYNQT_HOST_SUITES SYNQT_SCRIPT_SUITES SYNQT_SUITES_ELSEWHERE)
        string(FIND "${_synqt_dev_docs_text}" "tests/${_synqt_suite})" _synqt_found)
        if(_synqt_found EQUAL -1)
            list(APPEND _synqt_undocumented "${_synqt_suite}")
        endif()
    endforeach()
    if(_synqt_undocumented)
        list(JOIN _synqt_undocumented " " _synqt_undocumented_text)
        message(FATAL_ERROR
            "suites this tree runs that docs/development.md never mentions: "
            "${_synqt_undocumented_text}\n"
            "Add a row to its test suite table saying what each one proves.")
    endif()
endif()

# Publish the script suites for whoever runs the tree, so a workflow does not keep a second
# copy of this list and drift from it. The guard above is what makes this file the registry;
# this line is what lets CI read the registry instead of restating it.
list(JOIN SYNQT_SCRIPT_SUITES "\n" _synqt_script_suites_text)
file(WRITE "${CMAKE_BINARY_DIR}/script-suites.txt" "${_synqt_script_suites_text}\n")

foreach(_synqt_suite IN LISTS SYNQT_HOST_SUITES)
    add_subdirectory("${_synqt_suite}")
endforeach()
