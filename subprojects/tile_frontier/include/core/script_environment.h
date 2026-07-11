#ifndef TILE_FRONTIER_CORE_SCRIPT_ENVIRONMENT_H
#define TILE_FRONTIER_CORE_SCRIPT_ENVIRONMENT_H

#include <devils_script/system.h>

#include "entity_scope.h"

// Проектный holder devils_script::system: ЕДИНСТВЕННАЯ точка регистрации нативных геймплейных
// функций (скрипты обязаны их видеть) + парсер скриптов. Нативки трогают aesthetics-компоненты,
// поэтому system живёт в проекте, а не в движковом libs/act. system нужен ТОЛЬКО на парсе —
// скомпилированный container самодостаточен в runtime. Владелец — assets-система (живёт на время
// приложения, переживает parse_resources). См. [[devils-script-and-config]].

namespace tile_frontier {
namespace core {

struct script_environment {
  devils_script::system sys;
  script_environment();
};

}
}

#endif
