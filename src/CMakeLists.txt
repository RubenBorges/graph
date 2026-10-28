
set(GRAPH_SOURCES
     Process.cpp
     Wire.cpp
     WebSocketConnection.cpp
     main.cpp
     CentralCoordinatorIDGen.cpp
)
set(GRAPH_INCLUDES
     Process.hpp
     Wire.hpp
     WebSocketConnection.hpp
     WebSocketChannel.hpp
     CentralCoordinatorIDGen.hpp
)

add_executable(Graph ${GRAPH_SOURCES} ${GRAPH_INCLUDES})

target_include_directories(Graph PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(Graph SYSTEM PRIVATE ${Boost_INCLUDE_DIRS})
target_compile_definitions(Graph PRIVATE BOOST_ERROR_CODE_HEADER_ONLY)
target_link_libraries(Graph PRIVATE OpenSSL::SSL OpenSSL::Crypto)

# ---------------------------------------------------------
# Link dependencies
# ---------------------------------------------------------



install(TARGETS Graph
    RUNTIME DESTINATION bin
)