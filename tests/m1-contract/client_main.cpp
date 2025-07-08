// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// Compile/link smoke target for the consumer (ROLE replica): it gets only the
// Replica-side repc classes and their QML registration. That this links (and links
// without Qt Gui, which the Source helper needs) proves a client entity builds with
// no Source helper linked in, honoring "never link a service-only module into the
// client".

#include "catalog_replica.h"
#include "todo_replica.h"

int main()
{
    synqtRegisterTodoReplicas();
    synqtRegisterCatalogReplicas();
    return 0;
}
