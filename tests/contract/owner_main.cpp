// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// Compile/link smoke target for the owner (ROLE source): it gets the Source helpers
// and the Source-side repc classes only. That this links proves an owner entity
// builds without any Replica code.

#include "catalog_sourcehelper.h"
#include "todo_sourcehelper.h"

int main()
{
    TodoSourceHelper todo;
    CatalogSourceHelper catalog;
    synqtRegisterTodoSources();
    synqtRegisterCatalogSources();
    todo.setCount(1);
    return 0;
}
