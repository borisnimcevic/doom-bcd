file(GLOB DG_SRCS "*.c")

idf_component_register(SRCS ${DG_SRCS}
                       INCLUDE_DIRS "."
                       LDFRAGMENTS "linker.lf")

# id's 1993 C, run through a 2025 compiler — turn the noise off and allow the
# tentative-definition style the original relies on.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -DNORMALUNIX -DLINUX
    -fcommon
    -w
    -Wno-error
    -Wno-implicit-function-declaration
)
