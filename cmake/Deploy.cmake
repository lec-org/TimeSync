if(WIN32 AND NOT TIMESYNC_STATIC_QT)
    find_program(TIMESYNC_WINDEPLOYQT
        NAMES windeployqt windeployqt.exe
        HINTS
            "${QT_HOST_PATH}/bin"
            "${Qt6_DIR}/../../../bin"
    )

    if(TIMESYNC_WINDEPLOYQT)
        add_custom_target(deploy-portable
            COMMAND "${CMAKE_COMMAND}" -E rm -rf "${CMAKE_BINARY_DIR}/deploy"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/deploy"
            COMMAND "${CMAKE_COMMAND}" -E copy
                    "$<TARGET_FILE:TimeSync>"
                    "${CMAKE_BINARY_DIR}/deploy/TimeSync.exe"
            COMMAND "${TIMESYNC_WINDEPLOYQT}"
                    --release
                    --no-translations
                    --no-opengl-sw
                    --no-system-d3d-compiler
                    --no-svg
                    --skip-plugin-types iconengines,generic,networkinformation,imageformats
                    --dir "${CMAKE_BINARY_DIR}/deploy"
                    "$<TARGET_FILE:TimeSync>"
            DEPENDS TimeSync
            COMMENT "Deploying Qt runtime beside TimeSync.exe"
            VERBATIM
        )
    else()
        message(STATUS "windeployqt not found; deploy-portable target is unavailable")
    endif()
endif()
