# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# What an entity's own CMakeLists.txt can call, and the record of what each entity links.
#
#   synqt_entity_qt_modules(<entity> <component> [<component> ...])
#       Find more Qt modules at the project's Qt version and link them into the entity.
#
#   synqt_record_linked(<entity>)
#       At the end of configure, write what the entity links to
#       ${CMAKE_BINARY_DIR}/synqt-linked/<entity>.txt, one name per line. THIRD-PARTY-LICENSES
#       is generated from it.
#
# SYNQT_QT_VERSION is set by the generated build to project.qt_version.

function(synqt_entity_qt_modules entity)
    if(NOT TARGET ${entity})
        message(FATAL_ERROR "synqt_entity_qt_modules: there is no entity named '${entity}'")
    endif()
    if(NOT ARGN)
        message(FATAL_ERROR "synqt_entity_qt_modules: name at least one Qt component")
    endif()
    if(NOT SYNQT_QT_VERSION)
        message(FATAL_ERROR "synqt_entity_qt_modules: SYNQT_QT_VERSION is not set")
    endif()
    find_package(Qt6 ${SYNQT_QT_VERSION} REQUIRED COMPONENTS ${ARGN})
    foreach(component IN LISTS ARGN)
        target_link_libraries(${entity} PRIVATE Qt6::${component})
    endforeach()
endfunction()

function(synqt_record_linked entity)
    # A deferred call reads its arguments when it runs, so the values are written in now.
    set(file "${CMAKE_BINARY_DIR}/synqt-linked/${entity}.txt")
    cmake_language(EVAL CODE "
        cmake_language(DEFER DIRECTORY [[${CMAKE_SOURCE_DIR}]]
            CALL _synqt_record_linked [[${entity}]] [[${file}]])")
endfunction()

# Read once every directory has been configured, so links added by an entity's own
# CMakeLists.txt are seen. The project's own targets and imported libraries are followed
# through their interface links. The SynQt runtime libraries are not (what they link is
# known from the topology), and neither are Qt modules.
function(_synqt_record_linked entity file)
    get_target_property(queue ${entity} LINK_LIBRARIES)
    if(NOT queue)
        set(queue "")
    endif()
    set(followed "")
    set(recorded "")
    while(queue)
        list(POP_FRONT queue item)
        # A link added from another directory is bracketed by `::@(<id>)` and `::@`.
        if(item MATCHES "^::@")
            continue()
        endif()
        if(item MATCHES "^\\$<LINK_ONLY:(.+)>$")
            set(item "${CMAKE_MATCH_1}")
        endif()
        if(item MATCHES "^\\$<" OR item STREQUAL "" OR item IN_LIST followed)
            continue()
        endif()
        list(APPEND followed "${item}")
        if(item MATCHES "^SynQt")
            continue()
        endif()
        # A Qt module is named and not followed: its own dependencies are Qt's to list.
        if(item MATCHES "^Qt6?::")
            list(APPEND recorded "${item}")
            continue()
        endif()
        if(TARGET "${item}")
            get_target_property(imported "${item}" IMPORTED)
            get_target_property(aliased "${item}" ALIASED_TARGET)
            if(aliased)
                list(APPEND queue "${aliased}")
                continue()
            endif()
            if(imported)
                list(APPEND recorded "${item}")
            endif()
            get_target_property(interface "${item}" INTERFACE_LINK_LIBRARIES)
            if(interface)
                list(APPEND queue ${interface})
            endif()
        else()
            list(APPEND recorded "${item}")
        endif()
    endwhile()
    list(REMOVE_DUPLICATES recorded)
    list(SORT recorded)
    list(JOIN recorded "\n" text)
    file(WRITE "${file}" "${text}\n")
endfunction()
