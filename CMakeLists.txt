CMAKE_MINIMUM_REQUIRED(VERSION 3.6)

project(demex-canopen
    VERSION 2.0.0
    DESCRIPTION "Provides a simple demo showing use of canopen"
#    HOMEPAGE_URL "https://"
    LANGUAGES C
)

# Declare options
set(AFM_APP_DIR ${CMAKE_INSTALL_PREFIX}/redpesk CACHE PATH "Applications directory")
set(APP_DIR ${AFM_APP_DIR}/${PROJECT_NAME})

# Check dependencies
include(FindPkgConfig)
pkg_check_modules(
    deps
    REQUIRED
        json-c
        afb-binding>=4.1.0
        CANopenXchg
)

# generation of compiled version of info
set(j2c   ${CMAKE_SOURCE_DIR}/src/j2c.sh)
set(infoj ${CMAKE_SOURCE_DIR}/src/info.json)
set(infoc ${CMAKE_BINARY_DIR}/info.c)
add_custom_command(
    OUTPUT  ${infoc}
    COMMAND ${j2c} ${infoj} infotxt > ${infoc}
    DEPENDS ${infoj} ${j2c}
)

# sources of the binding
set(SRC
    ${CMAKE_SOURCE_DIR}/src/demex-canopen.c
    ${infoc}
)

add_library(demexco SHARED ${SRC})
target_compile_options(demexco PRIVATE ${deps_CFLAGS})
target_link_libraries(demexco ${deps_LDFLAGS})
set_target_properties(demexco PROPERTIES PREFIX "")

# This version script is a linker script which exports all symbols named "afbBinding*" and makes all the other symbols local only
pkg_get_variable(vscript afb-binding version_script)
if(vscript)
    target_link_options(demexco PRIVATE -Wl,--version-script=${vscript})
endif(vscript)

# Install
install(TARGETS demexco DESTINATION ${APP_DIR}/lib)
install(FILES rpconfig/manifest.yml DESTINATION ${APP_DIR}/.rpconfig)
