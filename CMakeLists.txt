# ============================================================================
#  Root CMakeLists.txt — SMS Forwarder v3.6.4
#  PlatformIO Arduino+ESP-IDF 双框架 模式下, PIO 走自己的 SCons, 但 ESP-IDF
#  组件扫描仍由本文件驱动. EXTRA_COMPONENT_DIRS 把本地 components/ +
#  managed_components/ 加进来.
# ============================================================================
cmake_minimum_required(VERSION 3.16.0)

## Note: components/ + managed_components/ 都会被 ESP-IDF 自动扫,不用 EXTRA_COMPONENT_DIRS
## USB Host 三件套通过 src/idf_component.yml 由 idf_component_manager 自动从注册表拉

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(sms_forwarder)
