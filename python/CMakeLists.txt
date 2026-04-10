# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

find_package(pybind11 REQUIRED)

set(MODULE_NAME daqiri)

pybind11_add_module(${MODULE_NAME}_python MODULE daqiri_common_pybind.cpp)
target_link_libraries(${MODULE_NAME}_python PRIVATE daqiri_common)
set_target_properties(${MODULE_NAME}_python PROPERTIES OUTPUT_NAME _${MODULE_NAME})

install(TARGETS ${MODULE_NAME}_python
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/python/${MODULE_NAME}
  COMPONENT daqiri-python
)

configure_file(
  ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/pybind11/__init__.py
  ${CMAKE_CURRENT_BINARY_DIR}/${MODULE_NAME}/__init__.py
  COPYONLY
)
install(
  FILES ${CMAKE_CURRENT_BINARY_DIR}/${MODULE_NAME}/__init__.py
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/python/${MODULE_NAME}
  COMPONENT daqiri-python
)

# UTILS

install(FILES tune_system.py
  DESTINATION bin
  PERMISSIONS OWNER_EXECUTE OWNER_WRITE OWNER_READ
  COMPONENT daqiri-utils
)
