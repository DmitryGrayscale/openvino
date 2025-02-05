# Copyright (C) 2018-2021 Intel Corporation
# SPDX-License-Identifier: Apache-2.0
#

set(TARGET_NAME "paddlepaddle_tests")

file(GLOB_RECURSE SRC ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)

add_executable(${TARGET_NAME} ${SRC})

target_link_libraries(${TARGET_NAME} PRIVATE frontend_shared_test_classes)

add_clang_format_target(${TARGET_NAME}_clang FOR_TARGETS ${TARGET_NAME})

install(TARGETS ${TARGET_NAME}
        RUNTIME DESTINATION tests
        COMPONENT tests
        EXCLUDE_FROM_ALL)

# Test model generating
ie_check_pip_package(paddlepaddle WARNING)

set(TEST_PADDLE_MODELS_DIRNAME test_model_zoo/paddle_test_models)
target_compile_definitions(${TARGET_NAME} PRIVATE -D TEST_PADDLE_MODELS_DIRNAME=\"${TEST_PADDLE_MODELS_DIRNAME}/\")

# If 'paddlepaddle' is not found, code will still be compiled
# but models will not be generated and tests will fail
# This is done this way for 'code style' and check cases - cmake shall pass, but CI machine doesn't need to have
# 'paddlepaddle' installed to check code style
if (paddlepaddle_FOUND)
    set(TEST_PADDLE_MODELS ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${TEST_PADDLE_MODELS_DIRNAME}/)
    set(GEN_WRAPPER ${CMAKE_CURRENT_SOURCE_DIR}/test_models/gen_wrapper.py)
    file(GLOB_RECURSE PADDLE_GEN_SCRIPTS ${CMAKE_CURRENT_SOURCE_DIR}/test_models/gen_scripts/generate_*.py)

    foreach(GEN_SCRIPT ${PADDLE_GEN_SCRIPTS})
        get_filename_component(FILE_WE ${GEN_SCRIPT} NAME_WE)
        set(OUT_DONE_FILE ${TEST_PADDLE_MODELS}/${FILE_WE}_done.txt)
        list(APPEND OUT_FILES ${OUT_DONE_FILE})
        add_custom_command(OUTPUT ${OUT_DONE_FILE}
                COMMAND ${PYTHON_EXECUTABLE}
                    ${GEN_WRAPPER}
                    ${GEN_SCRIPT}
                    ${TEST_PADDLE_MODELS}
                    ${OUT_DONE_FILE}
                DEPENDS ${GEN_WRAPPER} ${GEN_SCRIPT}
                JOB_POOL four_jobs
                COMMENT "Running ${FILE_WE}.py to generate PDPD model")
    endforeach()
    add_custom_target(paddlepaddle_test_models DEPENDS ${OUT_FILES})

    install(DIRECTORY ${TEST_PADDLE_MODELS}
            DESTINATION tests/${TEST_PADDLE_MODELS_DIRNAME}
            COMPONENT tests
            EXCLUDE_FROM_ALL)
else()
    # Produce warning message at build time as well
    add_custom_command(OUTPUT unable_build_paddle_models.txt
            COMMAND ${CMAKE_COMMAND}
            -E cmake_echo_color --red "Warning: Unable to generate PaddlePaddle test models. Running '${TARGET_NAME}' will likely fail"
            )
    add_custom_target(paddlepaddle_test_models DEPENDS unable_build_paddle_models.txt)
endif()

add_dependencies(${TARGET_NAME} paddlepaddle_test_models)
add_dependencies(${TARGET_NAME} paddlepaddle_ngraph_frontend)

# Fuzzy tests for PaddlePaddle use IE_CPU engine
if (ENABLE_MKL_DNN)
    add_dependencies(${TARGET_NAME} MKLDNNPlugin)
endif()