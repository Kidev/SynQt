# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# Every test suite that runs on the host kit, in one build tree and under one ctest. Each
# suite also stays a standalone project with its own run-*.sh.
#
# In the order the layers build on each other, so a failure reads in dependency order.
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
    api-inbound
    fix1-auction
    fix2-arena
    fix3-stall
    fix4-plaza
    url-routing
    remote-pages
    entity-test
    graphics
    privacy
    memory
    monitor
)

# Suites whose entry point is a generator, not CMake. Published below for CI.
set(SYNQT_SCRIPT_SUITES
    custom-provider  # compiles the skeletons from `synqt add provider`
    appgen-native    # compiles the entity mains appgen.py emits for a whole topology
    desktop-client   # builds the client QML as a native desktop app
    monitor-console  # scaffolds a monitor, builds it, and drives its console in a browser
    identity-picker  # three tabs of one browser through the development scope picker
    dev-exclusion    # configures the framework twice and reads the symbol tables
)

# Suites run by another workflow, and the directories under tests/ that are not suites.
set(SYNQT_SUITES_ELSEWHERE
    m0-transport         # WASM client and a native edge, in a browser; browser-matrix.yml
    wasm-quick3dphysics  # WASM only; wasm-proofs.yml
    plaza-browser        # the 3D plaza, `synqt dev` and a browser; wasm-proofs.yml
    site-home            # the built docs site's front page in a browser; docs.yml
    split-origin         # two sites in a real browser, no Qt at all; browser-matrix.yml
    designer             # the design editor in a browser, no Qt at all; tests.yml
    lib                  # shared shell helpers: sourced by the runners, or wrapped around a
                         # test (keyring-session.sh). Nothing to run on its own
    security             # the attack index, with nothing to run. The tests it names live
                         # beside the code they are about; tools/synqt/tests/test_security_index.py checks it
    local-network        # machine plumbing for split-origin: it edits hosts and trusts a
                         # development CA, so it is run by hand and never by CI
)

# Every directory with a CMakeLists.txt or a shell script must appear in exactly one of
# the three lists above. Configuring the tree enforces it.
file(GLOB _synqt_suite_files
    RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/*/CMakeLists.txt"
    "${CMAKE_CURRENT_SOURCE_DIR}/*/*.sh")
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

# Every registered suite must also appear in docs/development.md.
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

# Publish the script suites, so CI reads this list instead of copying it.
list(JOIN SYNQT_SCRIPT_SUITES "\n" _synqt_script_suites_text)
file(WRITE "${CMAKE_BINARY_DIR}/script-suites.txt" "${_synqt_script_suites_text}\n")

foreach(_synqt_suite IN LISTS SYNQT_HOST_SUITES)
    add_subdirectory("${_synqt_suite}")
endforeach()
