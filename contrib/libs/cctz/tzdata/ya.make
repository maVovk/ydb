LIBRARY()

WITHOUT_LICENSE_TEXTS()

VERSION(2025b)

LICENSE(Apache-2.0)

PEERDIR(
    contrib/libs/cctz
)

INCLUDE(ya.make.resources)

SRCS(
    GLOBAL factory.cpp
)

END()
