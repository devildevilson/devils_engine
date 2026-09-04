cmake_minimum_required(VERSION 3.30)

project(GN04_texture_generation VERSION 0.1.0 LANGUAGES CXX)

# Площадка headless ПО ПРИРОДЕ, а не по недоделке: всё, что она доказывает, — что результат остаётся
# на устройстве, поэтому окна у неё быть не должно. Наружу приезжает сводка на несколько сотен байт;
# картинку целиком можно попросить отдельно (`--dump`), и это уже для глаза, а не для конвейера.
add_executable(GN04_texture_generation src/main.cpp)

target_link_libraries(GN04_texture_generation PRIVATE
  devils_engine::painter
  devils_engine::utils
  devils_engine::options)

set_target_properties(GN04_texture_generation PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/subprojects/playgrounds/GN04_texture_generation/bin")

if (DEVILS_ENGINE_BUILD_TESTS)
  # Проверка ОХРАНЯЕМАЯ изнутри: без устройства она печатает это и выходит успешно, потому что
  # «устройства нет» — законный ответ, а не провал сборки.
  add_test(NAME GN04_texture_generation_verify
    COMMAND GN04_texture_generation --verify --size=512 --sites=64)
  set_tests_properties(GN04_texture_generation_verify PROPERTIES LABELS devils_engine)
endif()
