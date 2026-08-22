cmake_minimum_required(VERSION 3.30)

# Playground directories are independently buildable experiments. Add a directory here only after
# it owns an executable; README-only planned laboratories deliberately do not create empty targets.
add_subdirectory(common)
add_subdirectory(AU02_directional_coloration)
add_subdirectory(PF01_forward_plus)
add_subdirectory(PF02_shadows)
add_subdirectory(PF03_post_processing)
add_subdirectory(PF04_stencil_effects)
add_subdirectory(PF05_scene_effects)
