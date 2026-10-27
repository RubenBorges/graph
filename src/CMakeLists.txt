
set(GRAPH_SOURCES
     Process.cpp
     Wire.cpp
     main.cpp
     CentralCoordinatorIDGen.cpp
)
set(GRAPH_INCLUDES
     Process.hpp
     Wire.hpp
     CentralCoordinatorIDGen.hpp
)

add_executable(Graph ${GRAPH_SOURCES} ${GRAPH_INCLUDES})

target_include_directories(Graph PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# ---------------------------------------------------------
# Link dependencies
# ---------------------------------------------------------



install(TARGETS Graph
    RUNTIME DESTINATION bin
)