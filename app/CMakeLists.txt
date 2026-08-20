# SPDX-License-Identifier: GPL-3.0-only
add_executable(beatbench main.cpp)
target_link_libraries(beatbench PRIVATE beatbench_core Qt6::Widgets)
beatbench_set_warnings(beatbench)
set_target_properties(beatbench PROPERTIES
  WIN32_EXECUTABLE TRUE   # Windows 下不弹控制台
  MACOSX_BUNDLE TRUE
)
