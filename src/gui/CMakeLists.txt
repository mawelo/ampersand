# src/gui/CMakeLists.txt — Qt-Widgets-GUI "ampersand"
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets)
qt_standard_project_setup()

qt_add_executable(ampersand
    main.cpp
    controller.cpp controller.hpp
    main_window.cpp main_window.hpp
    grid_view.cpp grid_view.hpp
    block_panel.cpp block_panel.hpp
    catalog_dialog.cpp catalog_dialog.hpp
    library_panel.cpp library_panel.hpp
    tuner_view.cpp tuner_view.hpp
    meter_view.cpp meter_view.hpp
    io_meter_view.cpp io_meter_view.hpp
    theme.hpp)
target_link_libraries(ampersand PRIVATE qccore Qt6::Core Qt6::Gui Qt6::Widgets qc_hardening)
# Qt's moc output and Q_OBJECT boilerplate do not survive -Wold-style-cast/-Wconversion.
target_compile_options(ampersand PRIVATE -Wall -Wextra -Wshadow -Wnon-virtual-dtor -Werror)
set_target_properties(ampersand PROPERTIES
    INSTALL_RPATH "${CMAKE_PREFIX_PATH}/lib"
    BUILD_WITH_INSTALL_RPATH ON)
install(TARGETS ampersand RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

