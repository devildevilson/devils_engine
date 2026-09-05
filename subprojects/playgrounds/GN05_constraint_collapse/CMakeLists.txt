cmake_minimum_required(VERSION 3.30)

project(GN05_constraint_collapse VERSION 0.1.0 LANGUAGES CXX)

# Площадка headless: результат — данные, и проверяется он числами. Картинку можно попросить отдельно
# (`--dump`), и это уже для глаза, а не для конвейера.
add_executable(GN05_constraint_collapse src/main.cpp)

target_compile_definitions(GN05_constraint_collapse PRIVATE
  GN05_RESOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/resources")

target_link_libraries(GN05_constraint_collapse PRIVATE
  devils_engine::originator
  devils_engine::originator_config
  devils_engine::originator_lua
  devils_engine::utils
  devils_engine::options)

set_target_properties(GN05_constraint_collapse PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/subprojects/playgrounds/GN05_constraint_collapse/bin")

if (DEVILS_ENGINE_BUILD_TESTS)
  add_test(NAME GN05_constraint_collapse_verify
    COMMAND GN05_constraint_collapse --verify --size=64)
  set_tests_properties(GN05_constraint_collapse_verify PROPERTIES LABELS devils_engine)
endif()
