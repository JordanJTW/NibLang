configure_file(lit.site.cfg.py.template lit.site.cfg.py @ONLY)

add_custom_target(lit
        COMMAND lit "${CMAKE_CURRENT_BINARY_DIR}"
        DEPENDS compiler run)

# Ensure that `filecheck-ng` is available to the tests.
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(NIB_VENV_DIR "${CMAKE_BINARY_DIR}/venv")
set(NIB_VENV_PYTHON "${NIB_VENV_DIR}/bin/python")
set(FILECHECK_EXECUTABLE "${NIB_VENV_DIR}/bin/filecheck")

if(NOT EXISTS "${FILECHECK_EXECUTABLE}")
    message(STATUS "Setting up Python virtual environment...")

    execute_process(
        COMMAND ${Python3_EXECUTABLE} -m venv "${NIB_VENV_DIR}"
        RESULT_VARIABLE VENV_RES
    )
    if(NOT VENV_RES EQUAL 0)
        message(FATAL_ERROR "Failed to create Python virtual environment.")
    endif()

    execute_process(
        COMMAND "${NIB_VENV_PYTHON}" -m pip install --upgrade pip
        COMMAND "${NIB_VENV_PYTHON}" -m pip install -r "${CMAKE_CURRENT_SOURCE_DIR}/requirements.txt"
        RESULT_VARIABLE PIP_RES
    )
    if(NOT PIP_RES EQUAL 0)
        message(FATAL_ERROR "Failed to install Python test dependencies from requirements.txt.")
    endif()
endif()