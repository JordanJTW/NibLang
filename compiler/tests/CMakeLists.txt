configure_file(lit.site.cfg.py.template lit.site.cfg.py @ONLY)

add_custom_target(lit
        COMMAND lit "${CMAKE_CURRENT_BINARY_DIR}"
        DEPENDS compiler run)