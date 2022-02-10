RESOURCES_LIBRARY()
OWNER(heretic)
IF(USE_SYSTEM_ERROR_PRONE)
    MESSAGE(WARNING System Error Prone $USE_SYSTEM_ERROR_PRONE will be used)
ELSEIF(ERROR_PRONE_VERSION == "2.3.1")
    DECLARE_EXTERNAL_RESOURCE(ERROR_PRONE sbr:616853779)
ELSEIF(ERROR_PRONE_VERSION == "2.3.2")
    DECLARE_EXTERNAL_RESOURCE(ERROR_PRONE sbr:760800655)
ELSEIF(ERROR_PRONE_VERSION == "2.3.3")
    DECLARE_EXTERNAL_RESOURCE(ERROR_PRONE sbr:919320393)
ELSEIF(ERROR_PRONE_VERSION == "2.4.0")
    DECLARE_EXTERNAL_RESOURCE(ERROR_PRONE sbr:1585305794)
ELSEIF(ERROR_PRONE_VERSION == "2.6.0")
    DECLARE_EXTERNAL_RESOURCE(ERROR_PRONE sbr:2139890169)
ELSEIF(ERROR_PRONE_VERSION == "2.7.1")
    DECLARE_EXTERNAL_RESOURCE(ERROR_PRONE sbr:2202855601)
ELSEIF(ERROR_PRONE_VERSION == "2.10.0")
    DECLARE_EXTERNAL_RESOURCE(ERROR_PRONE sbr:2649935593)
ELSE()
    MESSAGE(FATAL_ERROR Unsupported Error Prone version: $ERROR_PRONE_VERSION)
ENDIF()
END()

RECURSE(
    2.10.0
    2.7.1
    2.3.1
)
