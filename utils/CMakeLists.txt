project(utils)

set(AbrtActionAnalyzeJava_SRCS abrt-action-analyze-java.c)

include(CheckIncludeFiles)

if (AJC_ENABLE_NLS)
    check_include_files(locale.h HAVE_LOCALE_H)
    if (HAVE_LOCALE_H)
        set (ENABLE_NLS true)
    endif (HAVE_LOCALE_H)
endif (AJC_ENABLE_NLS)

pkg_check_modules(PC_RPM REQUIRED rpm)

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/config.h.in ${CMAKE_CURRENT_BINARY_DIR}/config.h)

include_directories(${PC_SATYR_INCLUDE_DIRS})
include_directories(${PC_LIBREPORT_INCLUDE_DIRS})
include_directories(${PC_ABRT_INCLUDE_DIRS})
include_directories(${PC_RPM_INCLUDE_DIRS})

add_definitions(-DHAVE_CONFIG_H)
add_definitions(-DPACKAGE=\"${CMAKE_PROJECT_NAME}\")
add_definitions(-DLOCALEDIR=\"${LOCALE_INSTALL_DIR}\")
include_directories(${utils_BINARY_DIR})

add_executable(abrt-action-analyze-java ${AbrtActionAnalyzeJava_SRCS})
target_link_libraries(abrt-action-analyze-java ${PC_SATYR_LIBRARIES})
target_link_libraries(abrt-action-analyze-java ${PC_LIBREPORT_LIBRARIES})
target_link_libraries(abrt-action-analyze-java ${PC_ABRT_LIBRARIES})
target_link_libraries(abrt-action-analyze-java ${PC_RPM_LIBRARIES})

install(TARGETS abrt-action-analyze-java DESTINATION ${BIN_INSTALL_DIR})

install(FILES abrt-action-analyze-java.1 DESTINATION ${MAN_INSTALL_DIR}/man1)
