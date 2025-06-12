cmake_minimum_required(VERSION 3.22)

# -----------------------------------------------------------------------------
# Differential Expression Plugin
# -----------------------------------------------------------------------------
PROJECT("DifferentialExpressionPlugin")

# -----------------------------------------------------------------------------
# CMake Options
# -----------------------------------------------------------------------------
set(CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOMOC ON)

if(MSVC)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /DWIN32 /EHsc /MP /permissive- /Zc:__cplusplus")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /NODEFAULTLIB:LIBCMT")
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} /MDd")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /MD")
endif(MSVC)

# -----------------------------------------------------------------------------
# Set install directory
# -----------------------------------------------------------------------------
# Check if the directory to the ManiVault installation has been provided
# if(NOT DEFINED MV_INSTALL_DIR)
    # set(MV_INSTALL_DIR "" CACHE PATH "Directory where ManiVault is installed")
    # message(FATAL_ERROR "Please set MV_INSTALL_DIR to the directory where ManiVault is installed")
# endif()
# file(TO_CMAKE_PATH ${MV_INSTALL_DIR} MV_INSTALL_DIR)

# -----------------------------------------------------------------------------
# Dependencies
# -----------------------------------------------------------------------------
find_package(Qt6 COMPONENTS Widgets WebEngineWidgets REQUIRED)

find_package(ManiVault COMPONENTS Core PointData ClusterData CONFIG QUIET)

find_package(OpenMP)

if(OpenMP_CXX_FOUND)
    message(STATUS "Found OpenMP")
endif()

# -----------------------------------------------------------------------------
# Source files
# -----------------------------------------------------------------------------
# Define the plugin sources

set(MODEL
    src/TableModel.h
    src/TableModel.cpp
    src/TableSortFilterProxyModel.h
    src/TableSortFilterProxyModel.cpp
)

set(WIDGETS
    
    src/WordWrapHeaderView.h
    src/WordWrapHeaderView.cpp
    src/TableView.h
    src/TableView.cpp
    src/ButtonProgressBar.h
    src/ButtonProgressBar.cpp
)

set(UTIL
    src/ProgressManager.h
    src/ProgressManager.cpp
)

set(ACTIONS
    src/LoadedDatasetsAction.h
    src/LoadedDatasetsAction.cpp
    src/MultiTriggerAction.h
    src/MultiTriggerAction.cpp
)
set(PLUGIN_SOURCES
    src/DifferentialExpressionPlugin.h
    src/DifferentialExpressionPlugin.cpp
   
    src/DifferentialExpressionPlugin.json
)

set(QRESOURCES
    res/resources.qrc
)
QT6_ADD_RESOURCES(RESOURCE_FILES ${QRESOURCES})

set(PLUGIN_MOC_HEADERS
    src/DifferentialExpressionPlugin.h

    src/ButtonProgressBar.h
    src/TableModel.h
    src/TableSortFilterProxyModel.h
    src/TableView.h
    src/WordWrapHeaderView.h
)

source_group(Plugin FILES ${PLUGIN_SOURCES})
source_group(Model FILES ${MODEL})
source_group(Actions FILES ${ACTIONS})
source_group(Widget FILES ${WIDGETS})
source_group(Util FILES ${UTIL})

# -----------------------------------------------------------------------------
# CMake Target
# -----------------------------------------------------------------------------
# Create dynamic library for the plugin
add_library(${PROJECT_NAME} SHARED ${PLUGIN_SOURCES} ${RESOURCE_FILES} ${UTIL} ${WIDGETS} ${MODEL} ${ACTIONS})

# find_package(OpenMP REQUIRED)
# if(OpenMP_FOUND)
	# message(STATUS "OpenMP found")
	# target_link_libraries(${PROJECT_NAME} PRIVATE OpenMP::OpenMP_CXX)
	# target_compile_options(${PROJECT_NAME} PRIVATE ${OpenMP_CXX_FLAGS})
# else()
   # message(WARNING "OpenMP not found!")
# endif()

# qt_wrap_cpp(PLUGIN_MOC ${PLUGIN_MOC_HEADERS} TARGET ${PROJECT_NAME})
# target_sources(${PROJECT_NAME} PRIVATE ${PLUGIN_MOC})

# -----------------------------------------------------------------------------
# Target include directories
# -----------------------------------------------------------------------------
# Include ManiVault headers, including system data plugins
target_include_directories(${PROJECT_NAME} PRIVATE "${ManiVault_INCLUDE_DIR}")

# -----------------------------------------------------------------------------
# Target properties
# -----------------------------------------------------------------------------

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_20)
if(MV_UNITY_BUILD)
    set_target_properties(${PROJECT_NAME} PROPERTIES UNITY_BUILD ON)
endif()

# -----------------------------------------------------------------------------
# Target library linking
# -----------------------------------------------------------------------------
# Link to Qt libraries
target_link_libraries(${PROJECT_NAME} PRIVATE Qt6::Widgets)
target_link_libraries(${PROJECT_NAME} PRIVATE Qt6::WebEngineWidgets)

target_link_libraries(${PROJECT_NAME} PRIVATE ManiVault::Core)
target_link_libraries(${PROJECT_NAME} PRIVATE ManiVault::PointData)
target_link_libraries(${PROJECT_NAME} PRIVATE ManiVault::ClusterData)

if(OpenMP_CXX_FOUND)
    target_link_libraries(${PROJECT_NAME} PRIVATE OpenMP::OpenMP_CXX)
endif()


# -----------------------------------------------------------------------------
# Target installation
# -----------------------------------------------------------------------------
# Install the shared plugin libary to the "Plugins" folder in the ManiVault install directory
install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION Plugins COMPONENT PLUGINS # Windows .dll
    LIBRARY DESTINATION Plugins COMPONENT PLUGINS # Linux/Mac .so
)

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND "${CMAKE_COMMAND}"
        --install ${CMAKE_CURRENT_BINARY_DIR}
        --config $<CONFIGURATION>
        --prefix ${MV_INSTALL_DIR}/$<CONFIGURATION>
)

set_target_properties(${PROJECT_NAME}
    PROPERTIES
    FOLDER ViewPlugins
)


# -----------------------------------------------------------------------------
# Miscellaneous
# -----------------------------------------------------------------------------
# Automatically set the debug environment (command + working directory) for MSVC
if(MSVC)
    set_property(TARGET ${PROJECT_NAME} PROPERTY VS_DEBUGGER_WORKING_DIRECTORY $<IF:$<CONFIG:DEBUG>,${ManiVault_INSTALL_DIR}/Debug,$<IF:$<CONFIG:RELWITHDEBINFO>,${ManiVault_INSTALL_DIR}/RelWithDebInfo,${ManiVault_INSTALL_DIR}/Release>>)
    set_property(TARGET ${PROJECT_NAME} PROPERTY VS_DEBUGGER_COMMAND $<IF:$<CONFIG:DEBUG>,"${ManiVault_INSTALL_DIR}/Debug/ManiVault Studio.exe",$<IF:$<CONFIG:RELWITHDEBINFO>,"${ManiVault_INSTALL_DIR}/RelWithDebInfo/ManiVault Studio.exe","${ManiVault_INSTALL_DIR}/Release/ManiVault Studio.exe">>)
endif()
