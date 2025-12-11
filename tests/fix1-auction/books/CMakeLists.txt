# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The books entity of the gavel example, built as the separate binary it is in a real
# deployment. Its own directory because repc writes its output per directory, and this
# target owns the Books contract at ROLE source while the test beside it consumes it.

qt_add_executable(fix1_books books_entity.cpp)

# FORWARDS_SESSION, because the edge consumes this point and its calls carry the session it
# is acting for; the two halves have to be compiled with the same answer or the slot the
# consumer invokes is not the slot the owner declared.
synqt_add_contract(fix1_books ROLE source FORWARDS_SESSION
    SYN "${GAVEL}/generated/db/relational/books/Books.syn")

target_compile_definitions(fix1_books PRIVATE
    FIX1_CERT_DIR="${FIX1_CERTS}"
    FIX1_GAVEL_DIR="${GAVEL}"
)

target_link_libraries(fix1_books PRIVATE
    SynQtService
    Qt6::Core
    Qt6::Network
    Qt6::Qml
    Qt6::RemoteObjects
)
