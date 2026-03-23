idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES audio_driver ws_client tca9555 audio_codec)
